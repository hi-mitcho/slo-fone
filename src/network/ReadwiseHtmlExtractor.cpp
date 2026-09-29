#include "ReadwiseHtmlExtractor.h"

#include <cstdlib>

namespace {
constexpr char KEY[] = "\"html_content\"";
constexpr size_t KEY_LEN = sizeof(KEY) - 1;

bool isJsonWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
}  // namespace

void ReadwiseHtmlExtractor::appendCodepoint(const uint32_t codepoint) {
  // BMP only (Readwise's \uXXXX escapes never emit surrogate pairs for the
  // plain article text this feeds); a lone surrogate is dropped rather than
  // producing invalid UTF-8.
  if (codepoint == 0 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return;
  if (codepoint <= 0x7F) {
    html_.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    html_.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    html_.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    html_.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    html_.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    html_.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

bool ReadwiseHtmlExtractor::processByte(const char c) {
  switch (state_) {
    case State::SEARCHING_KEY: {
      if (c == KEY[keyIndex_]) {
        keyIndex_++;
        if (keyIndex_ == KEY_LEN) {
          state_ = State::AFTER_KEY;
          keyIndex_ = 0;
        }
      } else {
        // Restart the match from scratch, or from position 1 if this byte
        // happens to be the key's own first character.
        keyIndex_ = (c == KEY[0]) ? 1 : 0;
      }
      return true;
    }

    case State::AFTER_KEY: {
      if (isJsonWhitespace(c)) return true;
      if (c == ':') {
        state_ = State::AFTER_COLON;
        return true;
      }
      // Not our key after all (or malformed JSON) — abandon and let the
      // byte that broke the match be re-tried as the start of a fresh
      // search, in case it's itself an opening quote.
      state_ = State::SEARCHING_KEY;
      keyIndex_ = 0;
      return false;
    }

    case State::AFTER_COLON: {
      if (isJsonWhitespace(c)) return true;
      if (c == '"') {
        state_ = State::IN_VALUE;
        return true;
      }
      state_ = State::SEARCHING_KEY;
      keyIndex_ = 0;
      return false;
    }

    case State::IN_UNICODE_ESCAPE: {
      unicodeDigits_[unicodeCount_++] = c;
      if (unicodeCount_ == 4) {
        char buf[5] = {unicodeDigits_[0], unicodeDigits_[1], unicodeDigits_[2], unicodeDigits_[3], '\0'};
        appendCodepoint(static_cast<uint32_t>(strtoul(buf, nullptr, 16)));
        unicodeCount_ = 0;
        state_ = State::IN_VALUE;
      }
      return true;
    }

    case State::IN_ESCAPE: {
      state_ = State::IN_VALUE;
      switch (c) {
        case '"':
          html_.push_back('"');
          break;
        case '\\':
          html_.push_back('\\');
          break;
        case '/':
          html_.push_back('/');
          break;
        case 'n':
          html_.push_back('\n');
          break;
        case 't':
          html_.push_back('\t');
          break;
        case 'r':
          html_.push_back('\r');
          break;
        case 'b':
          html_.push_back('\b');
          break;
        case 'f':
          html_.push_back('\f');
          break;
        case 'u':
          unicodeCount_ = 0;
          state_ = State::IN_UNICODE_ESCAPE;
          break;
        default:
          html_.push_back(c);  // malformed escape: keep the literal character
          break;
      }
      return true;
    }

    case State::IN_VALUE: {
      if (c == '\\') {
        state_ = State::IN_ESCAPE;
      } else if (c == '"') {
        done_ = true;
      } else {
        html_.push_back(c);
      }
      return true;
    }
  }
  return true;
}

void ReadwiseHtmlExtractor::feed(const char* data, const size_t len) {
  for (size_t i = 0; i < len && !done_;) {
    if (processByte(data[i])) i++;
  }
}
