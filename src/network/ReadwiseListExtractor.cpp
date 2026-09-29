#include "ReadwiseListExtractor.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
constexpr char RESULTS_KEY[] = "\"results\"";
constexpr size_t RESULTS_KEY_LEN = sizeof(RESULTS_KEY) - 1;

// Per-field caps: bounds each captured value to a pre-reserved buffer so
// capturing a field never reallocates, regardless of how long the server's
// value actually is (excess characters are consumed, just not stored).
// Generous for what these fields are actually used for (list row label,
// author byline, a summary already word-wrapped over ~12 lines elsewhere).
constexpr size_t ID_CAP = 40;
constexpr size_t TITLE_CAP = 200;
constexpr size_t AUTHOR_CAP = 100;
constexpr size_t SUMMARY_CAP = 600;
constexpr size_t LOCATION_CAP = 16;
constexpr size_t CATEGORY_CAP = 16;

// Runaway-response guard: this is a byte-count safety net (parsing itself
// has negligible peak memory now), not a heap-pressure one -- stops a
// malformed or pathological response from being fed forever.
constexpr size_t MAX_TOTAL_BYTES = 512 * 1024;

bool isJsonWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
}  // namespace

size_t ReadwiseListExtractor::capFor(const Field f) {
  switch (f) {
    case Field::ID:
      return ID_CAP;
    case Field::TITLE:
      return TITLE_CAP;
    case Field::AUTHOR:
      return AUTHOR_CAP;
    case Field::SUMMARY:
      return SUMMARY_CAP;
    case Field::LOCATION:
      return LOCATION_CAP;
    case Field::CATEGORY:
      return CATEGORY_CAP;
    case Field::WORD_COUNT:
    case Field::NONE:
      return 0;
  }
  return 0;
}

ReadwiseListExtractor::ReadwiseListExtractor(std::vector<ReadwiseArticle>& outArticles) : articles_(outArticles) {
  current_.id.reserve(ID_CAP);
  current_.title.reserve(TITLE_CAP);
  current_.author.reserve(AUTHOR_CAP);
  current_.summary.reserve(SUMMARY_CAP);
  current_.location.reserve(LOCATION_CAP);
  category_.reserve(CATEGORY_CAP);
}

std::string* ReadwiseListExtractor::currentTarget() {
  switch (currentField_) {
    case Field::ID:
      return &current_.id;
    case Field::TITLE:
      return &current_.title;
    case Field::AUTHOR:
      return &current_.author;
    case Field::SUMMARY:
      return &current_.summary;
    case Field::LOCATION:
      return &current_.location;
    case Field::CATEGORY:
      return &category_;
    case Field::WORD_COUNT:
    case Field::NONE:
      return nullptr;
  }
  return nullptr;
}

void ReadwiseListExtractor::appendByteToValue(const char c) {
  std::string* target = currentTarget();
  if (target && target->size() < capFor(currentField_)) target->push_back(c);
}

void ReadwiseListExtractor::appendCodepointToValue(const uint32_t codepoint) {
  // BMP only, matching ReadwiseHtmlExtractor's own \uXXXX handling -- these
  // fields are plain titles/summaries/etc., never emoji-heavy article body
  // text with surrogate pairs.
  if (codepoint == 0 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return;
  if (codepoint <= 0x7F) {
    appendByteToValue(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    appendByteToValue(static_cast<char>(0xC0 | (codepoint >> 6)));
    appendByteToValue(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    appendByteToValue(static_cast<char>(0xE0 | (codepoint >> 12)));
    appendByteToValue(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    appendByteToValue(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

void ReadwiseListExtractor::finishKey() {
  struct NamedField {
    const char* name;
    Field field;
  };
  static constexpr NamedField kFields[] = {
      {"id", Field::ID},
      {"title", Field::TITLE},
      {"author", Field::AUTHOR},
      {"summary", Field::SUMMARY},
      {"location", Field::LOCATION},
      {"category", Field::CATEGORY},
      {"word_count", Field::WORD_COUNT},
  };
  currentField_ = Field::NONE;
  for (const auto& f : kFields) {
    if (keyLen_ == strlen(f.name) && memcmp(keyBuf_, f.name, keyLen_) == 0) {
      currentField_ = f.field;
      break;
    }
  }
}

void ReadwiseListExtractor::finishObject() {
  if (current_.location != "archive" && current_.location != "feed" && category_ != "highlight" &&
      category_ != "note" && !current_.id.empty()) {
    articles_.push_back(std::move(current_));
  }
  current_ = ReadwiseArticle{};
  category_.clear();
  current_.id.reserve(ID_CAP);
  current_.title.reserve(TITLE_CAP);
  current_.author.reserve(AUTHOR_CAP);
  current_.summary.reserve(SUMMARY_CAP);
  current_.location.reserve(LOCATION_CAP);
}

bool ReadwiseListExtractor::processByte(const char c) {
  switch (state_) {
    case State::SEEK_RESULTS_KEY: {
      if (c == RESULTS_KEY[resultsKeyIndex_]) {
        resultsKeyIndex_++;
        if (resultsKeyIndex_ == RESULTS_KEY_LEN) {
          state_ = State::AFTER_RESULTS_KEY;
          resultsKeyIndex_ = 0;
        }
      } else {
        resultsKeyIndex_ = (c == RESULTS_KEY[0]) ? 1 : 0;
      }
      return true;
    }

    case State::AFTER_RESULTS_KEY: {
      if (isJsonWhitespace(c)) return true;
      if (c == ':') {
        state_ = State::AFTER_RESULTS_COLON;
        return true;
      }
      state_ = State::SEEK_RESULTS_KEY;
      resultsKeyIndex_ = 0;
      return false;
    }

    case State::AFTER_RESULTS_COLON: {
      if (isJsonWhitespace(c)) return true;
      if (c == '[') {
        state_ = State::IN_ARRAY;
        return true;
      }
      state_ = State::SEEK_RESULTS_KEY;
      resultsKeyIndex_ = 0;
      return false;
    }

    case State::IN_ARRAY: {
      if (isJsonWhitespace(c) || c == ',') return true;
      if (c == '{') {
        state_ = State::IN_OBJECT;
        return true;
      }
      if (c == ']') {
        done_ = true;
        return true;
      }
      error_ = true;
      done_ = true;
      return true;
    }

    case State::IN_OBJECT: {
      if (isJsonWhitespace(c) || c == ',') return true;
      if (c == '"') {
        keyLen_ = 0;
        state_ = State::IN_KEY_STRING;
        return true;
      }
      if (c == '}') {
        finishObject();
        state_ = State::IN_ARRAY;
        return true;
      }
      error_ = true;
      done_ = true;
      return true;
    }

    case State::IN_KEY_STRING: {
      if (c == '"') {
        finishKey();
        state_ = State::AFTER_KEY;
        return true;
      }
      if (keyLen_ < sizeof(keyBuf_)) keyBuf_[keyLen_++] = c;
      return true;
    }

    case State::AFTER_KEY: {
      if (isJsonWhitespace(c)) return true;
      if (c == ':') {
        state_ = State::BEFORE_VALUE;
        return true;
      }
      error_ = true;
      done_ = true;
      return true;
    }

    case State::BEFORE_VALUE: {
      if (isJsonWhitespace(c)) return true;
      if (c == '"') {
        state_ = State::IN_VALUE_STRING;
        return true;
      }
      if (c == '{' || c == '[') {
        skipDepth_ = 1;
        state_ = State::SKIP_VALUE;
        return true;
      }
      if ((c >= '0' && c <= '9') || c == '-') {
        numberLen_ = 0;
        if (numberLen_ < sizeof(numberBuf_) - 1) numberBuf_[numberLen_++] = c;
        state_ = State::IN_VALUE_NUMBER;
        return true;
      }
      if (c == 't' || c == 'f' || c == 'n') {
        state_ = State::IN_VALUE_LITERAL;
        return true;
      }
      error_ = true;
      done_ = true;
      return true;
    }

    case State::IN_VALUE_STRING: {
      if (c == '\\') {
        state_ = State::IN_VALUE_STRING_ESCAPE;
      } else if (c == '"') {
        currentField_ = Field::NONE;
        state_ = State::IN_OBJECT;
      } else {
        appendByteToValue(c);
      }
      return true;
    }

    case State::IN_VALUE_STRING_ESCAPE: {
      state_ = State::IN_VALUE_STRING;
      switch (c) {
        case '"':
          appendByteToValue('"');
          break;
        case '\\':
          appendByteToValue('\\');
          break;
        case '/':
          appendByteToValue('/');
          break;
        case 'n':
          appendByteToValue('\n');
          break;
        case 't':
          appendByteToValue('\t');
          break;
        case 'r':
          appendByteToValue('\r');
          break;
        case 'b':
          appendByteToValue('\b');
          break;
        case 'f':
          appendByteToValue('\f');
          break;
        case 'u':
          unicodeCount_ = 0;
          state_ = State::IN_VALUE_STRING_UNICODE;
          break;
        default:
          appendByteToValue(c);
          break;
      }
      return true;
    }

    case State::IN_VALUE_STRING_UNICODE: {
      unicodeDigits_[unicodeCount_++] = c;
      if (unicodeCount_ == 4) {
        char buf[5] = {unicodeDigits_[0], unicodeDigits_[1], unicodeDigits_[2], unicodeDigits_[3], '\0'};
        appendCodepointToValue(static_cast<uint32_t>(strtoul(buf, nullptr, 16)));
        unicodeCount_ = 0;
        state_ = State::IN_VALUE_STRING;
      }
      return true;
    }

    case State::IN_VALUE_NUMBER: {
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
        if (numberLen_ < sizeof(numberBuf_) - 1) numberBuf_[numberLen_++] = c;
        return true;
      }
      numberBuf_[numberLen_] = '\0';
      if (currentField_ == Field::WORD_COUNT) current_.wordCount = atoi(numberBuf_);
      currentField_ = Field::NONE;
      state_ = State::IN_OBJECT;
      return false;
    }

    case State::IN_VALUE_LITERAL: {
      if (c >= 'a' && c <= 'z') return true;
      currentField_ = Field::NONE;
      state_ = State::IN_OBJECT;
      return false;
    }

    case State::SKIP_VALUE: {
      if (c == '"') {
        state_ = State::SKIP_VALUE_STRING;
      } else if (c == '{' || c == '[') {
        skipDepth_++;
      } else if (c == '}' || c == ']') {
        skipDepth_--;
        if (skipDepth_ == 0) {
          currentField_ = Field::NONE;
          state_ = State::IN_OBJECT;
        }
      }
      return true;
    }

    case State::SKIP_VALUE_STRING: {
      if (c == '\\') {
        state_ = State::SKIP_VALUE_STRING_ESCAPE;
      } else if (c == '"') {
        state_ = State::SKIP_VALUE;
      }
      return true;
    }

    case State::SKIP_VALUE_STRING_ESCAPE: {
      state_ = State::SKIP_VALUE_STRING;
      return true;
    }
  }
  return true;
}

void ReadwiseListExtractor::feed(const char* data, const size_t len) {
  if (done_) return;
  totalBytes_ += len;
  if (totalBytes_ > MAX_TOTAL_BYTES) {
    error_ = true;
    done_ = true;
    return;
  }
  for (size_t i = 0; i < len && !done_;) {
    if (processByte(data[i])) i++;
  }
}
