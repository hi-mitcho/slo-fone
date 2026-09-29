#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ReadwiseClient.h"

// Incrementally parses a Readwise Reader /list/ JSON response's "results"
// array directly from the byte stream as chunks arrive via feed(), building
// ReadwiseArticle entries without ever buffering the response body. Applies
// the same client-side filtering listUnarchived() always has (archive/feed
// locations, highlight/note categories, empty ids all dropped).
//
// Why this exists (see ReadwiseClient.cpp git history): buffer-then-parse
// (accumulate the body into one std::string, then deserializeJson()) needs a
// single contiguous allocation as large as the response. On this device,
// that reliably failed under real conditions where total free heap looked
// fine but the largest contiguous block did not — confirmed on hardware,
// where no growth-step tuning of the buffered approach could work around it,
// since eventually the buffer needs one contiguous block bigger than
// whatever's actually available. Streaming avoids the problem entirely: peak
// extra memory is one in-flight article's small, pre-reserved field buffers
// (each capped — see the *_CAP constants in the .cpp) plus the output
// vector, regardless of response size.
//
// Not a general JSON parser: only recognizes the flat field names
// ReadwiseArticle needs inside each "results" element (id/title/author/
// summary/location/category/word_count); any other key's value (including
// nested objects/arrays) is skipped structurally without being interpreted.
// Field order and the presence of other top-level response keys don't
// matter. Everything after the results array closes is ignored.
//
// Host-testable: no Arduino/network dependencies.
class ReadwiseListExtractor {
 public:
  // outArticles is appended to (not cleared) as each object completes;
  // caller clears it first if that's not the desired behavior.
  explicit ReadwiseListExtractor(std::vector<ReadwiseArticle>& outArticles);

  void feed(const char* data, size_t len);

  // True once the results array has closed, or a malformed/oversized
  // response has caused this to bail early — check error() to tell them
  // apart. The caller should stop feeding once this is true.
  bool done() const { return done_; }
  // True if feed() stopped early on malformed JSON or the runaway-response
  // byte cap, rather than a clean end of the results array.
  bool error() const { return error_; }

 private:
  enum class State : uint8_t {
    SEEK_RESULTS_KEY,
    AFTER_RESULTS_KEY,
    AFTER_RESULTS_COLON,
    IN_ARRAY,
    IN_OBJECT,
    IN_KEY_STRING,
    AFTER_KEY,
    BEFORE_VALUE,
    IN_VALUE_STRING,
    IN_VALUE_STRING_ESCAPE,
    IN_VALUE_STRING_UNICODE,
    IN_VALUE_NUMBER,
    IN_VALUE_LITERAL,
    SKIP_VALUE,
    SKIP_VALUE_STRING,
    SKIP_VALUE_STRING_ESCAPE,
  };

  enum class Field : uint8_t { NONE, ID, TITLE, AUTHOR, SUMMARY, LOCATION, CATEGORY, WORD_COUNT };

  // Same true/false-return convention as ReadwiseHtmlExtractor::processByte:
  // false means "re-run this byte, state has changed since consuming it
  // wasn't valid/applicable here".
  bool processByte(char c);
  void appendCodepointToValue(uint32_t codepoint);
  void appendByteToValue(char c);
  void finishKey();
  void finishObject();
  std::string* currentTarget();
  static size_t capFor(Field f);

  std::vector<ReadwiseArticle>& articles_;
  State state_ = State::SEEK_RESULTS_KEY;
  bool done_ = false;
  bool error_ = false;
  size_t totalBytes_ = 0;

  size_t resultsKeyIndex_ = 0;

  char keyBuf_[24] = {};
  size_t keyLen_ = 0;
  Field currentField_ = Field::NONE;

  char unicodeDigits_[4] = {};
  int unicodeCount_ = 0;

  char numberBuf_[16] = {};
  size_t numberLen_ = 0;

  size_t skipDepth_ = 0;

  ReadwiseArticle current_;
  std::string category_;
};
