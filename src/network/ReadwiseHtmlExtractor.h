#pragma once

#include <cstddef>
#include <string>

// Pulls the "html_content" field out of a Readwise Reader /list/ JSON
// response as raw HTML, without buffering the whole (JSON-escaped) response
// body. Scans incoming bytes for the literal key `"html_content"`, then
// tolerates arbitrary whitespace around the colon and the opening quote
// (servers aren't guaranteed to emit minified JSON) before JSON-unescaping
// the string value directly into `html` as chunks arrive via feed(). Field
// order elsewhere in the JSON document doesn't matter since nothing but this
// one key is inspected.
//
// Host-testable: no Arduino/network dependencies (see
// test/readwise_html_extractor/).
class ReadwiseHtmlExtractor {
 public:
  void feed(const char* data, size_t len);

  // True once the closing (unescaped) quote of the html_content value has
  // been seen. The caller should stop feeding once this is true — there's
  // nothing left to extract.
  bool done() const { return done_; }

  const std::string& html() const { return html_; }

  // Current size of the value captured so far — lets a caller bound growth
  // (e.g. abort on an oversized or low-memory response) without waiting for
  // done().
  size_t size() const { return html_.size(); }

  // Hands ownership of the captured value to the caller, leaving this
  // extractor's copy empty. Avoids holding two full copies of a large
  // article body momentarily (this extractor's and the caller's) the way a
  // plain `outHtml = extractor.html()` copy would.
  std::string takeHtml() { return std::move(html_); }

 private:
  enum class State : uint8_t {
    SEARCHING_KEY,
    AFTER_KEY,     // matched "html_content", scanning whitespace + ':'
    AFTER_COLON,   // matched the colon, scanning whitespace + opening '"'
    IN_VALUE,
    IN_ESCAPE,
    IN_UNICODE_ESCAPE,
  };

  // Processes one byte. Returns true if the byte was consumed, false if the
  // caller should re-run the same byte through the (now different) state —
  // used when abandoning a partial key/colon/quote match part-way through,
  // since the byte that broke the match may itself start the next attempt.
  bool processByte(char c);
  void appendCodepoint(uint32_t codepoint);

  State state_ = State::SEARCHING_KEY;
  size_t keyIndex_ = 0;
  bool done_ = false;
  std::string html_;
  char unicodeDigits_[4] = {};
  int unicodeCount_ = 0;
};
