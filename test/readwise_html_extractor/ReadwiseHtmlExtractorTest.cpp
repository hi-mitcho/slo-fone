#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "src/network/ReadwiseHtmlExtractor.h"

namespace {

std::string extract(const char* json) {
  ReadwiseHtmlExtractor extractor;
  extractor.feed(json, strlen(json));
  return extractor.html();
}

}  // namespace

TEST(ReadwiseHtmlExtractor, ExtractsSimpleValue) {
  const char* json = R"({"id":"abc","html_content":"<p>hello</p>","title":"x"})";
  ReadwiseHtmlExtractor extractor;
  extractor.feed(json, strlen(json));
  EXPECT_TRUE(extractor.done());
  EXPECT_EQ(extractor.html(), "<p>hello</p>");
}

TEST(ReadwiseHtmlExtractor, StopsAtClosingQuoteNotFurtherContent) {
  const char* json = R"({"html_content":"<p>a</p>","other":"should not appear, even a quote\" here"})";
  EXPECT_EQ(extract(json), "<p>a</p>");
}

TEST(ReadwiseHtmlExtractor, KeyOrderDoesNotMatter) {
  const char* json = R"({"count":1,"results":[{"id":"x","title":"T","html_content":"<div>body</div>"}]})";
  EXPECT_EQ(extract(json), "<div>body</div>");
}

TEST(ReadwiseHtmlExtractor, NoMarkerPresentLeavesEmptyNotDone) {
  const char* json = R"({"id":"abc","title":"no content field here"})";
  ReadwiseHtmlExtractor extractor;
  extractor.feed(json, strlen(json));
  EXPECT_FALSE(extractor.done());
  EXPECT_EQ(extractor.html(), "");
}

TEST(ReadwiseHtmlExtractor, JsonEscapeSequences) {
  const char* json = R"({"html_content":"a\"b\\c\/d\ne\tf"})";
  EXPECT_EQ(extract(json), std::string("a\"b\\c/d\ne\tf"));
}

TEST(ReadwiseHtmlExtractor, UnicodeEscapeDecodedToUtf8) {
  // JSON \uXXXX escapes for U+00E9 (e-acute) and U+4E2D (a CJK ideograph),
  // written as literal backslash-u-hex text (not source-encoded UTF-8) so
  // this exercises the IN_UNICODE_ESCAPE decode path, not raw passthrough.
  const char* json = "{\"html_content\":\"caf\\u00e9 \\u4e2d\"}";
  const std::string result = extract(json);
  // UTF-8 for U+00E9 is 0xC3 0xA9; for U+4E2D is 0xE4 0xB8 0xAD.
  EXPECT_EQ(result, "caf\xC3\xA9 \xE4\xB8\xAD");
}

TEST(ReadwiseHtmlExtractor, RawUtf8PassesThroughUnchanged) {
  const char* json = R"({"html_content":"café 中"})";
  EXPECT_EQ(extract(json), "café 中");
}

TEST(ReadwiseHtmlExtractor, HtmlWithEmbeddedEscapedQuotesInAttributes) {
  const char* json = R"({"html_content":"<a href=\"https://x.com\">link</a>"})";
  EXPECT_EQ(extract(json), R"(<a href="https://x.com">link</a>)");
}

TEST(ReadwiseHtmlExtractor, ChunkedFeedingMatchesWholeFeed) {
  const char* json = R"({"id":"x","html_content":"<p>Some \"quoted\" text with é accents &amp; more</p>"})";
  const std::string reference = extract(json);

  for (size_t chunkSize = 1; chunkSize <= 9; ++chunkSize) {
    ReadwiseHtmlExtractor extractor;
    const size_t len = strlen(json);
    for (size_t offset = 0; offset < len; offset += chunkSize) {
      const size_t remaining = len - offset;
      const size_t feedLen = remaining < chunkSize ? remaining : chunkSize;
      extractor.feed(json + offset, feedLen);
    }
    EXPECT_EQ(extractor.html(), reference) << "chunkSize=" << chunkSize;
    EXPECT_TRUE(extractor.done()) << "chunkSize=" << chunkSize;
  }
}

TEST(ReadwiseHtmlExtractor, MarkerSplitAcrossChunkBoundary) {
  const char* json = R"({"html_content":"<p>x</p>"})";
  const size_t len = strlen(json);
  for (size_t split = 0; split <= len; ++split) {
    ReadwiseHtmlExtractor extractor;
    if (split > 0) extractor.feed(json, split);
    if (split < len) extractor.feed(json + split, len - split);
    EXPECT_TRUE(extractor.done()) << "split=" << split;
    EXPECT_EQ(extractor.html(), "<p>x</p>") << "split=" << split;
  }
}

TEST(ReadwiseHtmlExtractor, NearMissMarkerDoesNotFalsePositive) {
  // "html_content_extra" contains the marker as a strict prefix but isn't
  // followed by the expected ':"' — must not be mistaken for the real field.
  const char* json = R"({"html_content_extra":"nope","html_content":"<p>real</p>"})";
  EXPECT_EQ(extract(json), "<p>real</p>");
}

TEST(ReadwiseHtmlExtractor, EmptyHtmlContent) {
  const char* json = R"({"html_content":""})";
  ReadwiseHtmlExtractor extractor;
  extractor.feed(json, strlen(json));
  EXPECT_TRUE(extractor.done());
  EXPECT_EQ(extractor.html(), "");
}

TEST(ReadwiseHtmlExtractor, SpaceAfterColon) {
  // Readwise's actual API response is pretty-printed, not minified: a space
  // follows every colon. This is the bug that shipped in v1 — the marker
  // match required an immediately-adjacent quote after the colon.
  const char* json = R"({"id": "abc", "html_content": "<p>spaced</p>", "title": "x"})";
  EXPECT_EQ(extract(json), "<p>spaced</p>");
}

TEST(ReadwiseHtmlExtractor, WhitespaceBeforeColonAndTabsAndNewlines) {
  const char* json = "{\"html_content\"\t:\n  \"<p>x</p>\"}";
  EXPECT_EQ(extract(json), "<p>x</p>");
}

TEST(ReadwiseHtmlExtractor, PrettyPrintedMultilineResponse) {
  const char* json =
      "{\n"
      "  \"count\": 1,\n"
      "  \"results\": [\n"
      "    {\n"
      "      \"id\": \"01m30c98kbz9tatabcd8g1f52k\",\n"
      "      \"title\": \"An Article\",\n"
      "      \"html_content\": \"<div><p>Hello <b>world</b></p></div>\"\n"
      "    }\n"
      "  ]\n"
      "}";
  EXPECT_EQ(extract(json), "<div><p>Hello <b>world</b></p></div>");
}

TEST(ReadwiseHtmlExtractor, AlmostKeyWithSpaceStillRejected) {
  // "html_content_extra" followed by a colon must not be mistaken for the
  // real key even with whitespace variation.
  const char* json = R"({"html_content_extra" : "nope", "html_content" : "<p>real</p>"})";
  EXPECT_EQ(extract(json), "<p>real</p>");
}

TEST(ReadwiseHtmlExtractor, KeyFollowedByNonColonIsNotAMatch) {
  // Pathological but must not hang or false-positive: the key text appears
  // but isn't actually followed by a colon at all before the real field.
  const char* json = R"({"note": "see html_content here", "html_content":"<p>ok</p>"})";
  EXPECT_EQ(extract(json), "<p>ok</p>");
}

TEST(ReadwiseHtmlExtractor, ChunkedFeedingWithSpacedJson) {
  const char* json = R"({"id": "x", "html_content": "<p>Some \"quoted\" text with é accents</p>"})";
  const std::string reference = extract(json);

  for (size_t chunkSize = 1; chunkSize <= 9; ++chunkSize) {
    ReadwiseHtmlExtractor extractor;
    const size_t len = strlen(json);
    for (size_t offset = 0; offset < len; offset += chunkSize) {
      const size_t remaining = len - offset;
      const size_t feedLen = remaining < chunkSize ? remaining : chunkSize;
      extractor.feed(json + offset, feedLen);
    }
    EXPECT_EQ(extractor.html(), reference) << "chunkSize=" << chunkSize;
    EXPECT_TRUE(extractor.done()) << "chunkSize=" << chunkSize;
  }
}

TEST(ReadwiseHtmlExtractor, TruncatedInputNoCrash) {
  const char* truncated[] = {
      R"({"html_content":"unterm)",
      R"({"html_content":"a\)",
      R"({"html)",
      R"({"html_content":"a\u00)",
  };
  for (const char* json : truncated) {
    ReadwiseHtmlExtractor extractor;
    extractor.feed(json, strlen(json));
    // No crash is the pass condition; a truncated/partial value is fine.
  }
  SUCCEED();
}
