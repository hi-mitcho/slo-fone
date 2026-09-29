#include "ReadwiseClient.h"

#include <HalMemory.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "ReadwiseHtmlExtractor.h"
#include "ReadwiseListExtractor.h"

int ReadwiseClient::lastHttpCode = 0;

namespace {

// wolfSSL uses the default allocator, which can use PSRAM on supported builds.
// Keep a free-space floor and room for a full TLS record when the server does
// not negotiate our smaller record limit. These are preflight margins, not a
// guarantee that a handshake will fit.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

// Runaway-safety net for fetchHtmlContent's streaming extraction: checked
// every chunk while the article body accumulates in RAM (see the comment at
// its call site). MIN_FREE_DURING_EXTRACTION is lower than MIN_FREE_FOR_TLS
// because it's measured mid-connection, after wolfSSL's own session state is
// already live and counted against free heap, not as a preflight check.
//
// freeBytes alone doesn't catch everything: extractor.feed() grows html_ one
// push_back() at a time, and each reallocation needs a single NEW contiguous
// block at least as large as the next capacity while the old one is still
// live for the copy -- that can fail even when total free bytes look fine if
// the heap is fragmented into many smaller blocks. largestBlockBytes is the
// metric that actually bounds a single allocation (same reasoning
// insufficientHeap() already applies to the TLS handshake itself); checked
// here too, generously, since we can't know the next reallocation's exact
// size from outside std::string's growth policy.
constexpr size_t MAX_ARTICLE_HTML_BYTES = 128 * 1024;
constexpr size_t MIN_FREE_DURING_EXTRACTION = 15000;
constexpr size_t MIN_BLOCK_DURING_EXTRACTION = 20000;

bool insufficientHeap() {
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < MIN_FREE_FOR_TLS || heap.largestBlockBytes < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("READWISE",
            "Insufficient allocatable heap for TLS handshake: %zu bytes free (need %u), %zu max alloc (need %u)",
            heap.freeBytes, MIN_FREE_FOR_TLS, heap.largestBlockBytes, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

void applyAuthHeader(freeink::SecureHttpClient& http) {
  http.addHeader("Authorization", std::string("Token ") + SETTINGS.articleModuleToken);
}

}  // namespace

ReadwiseClient::Error ReadwiseClient::listUnarchived(std::vector<ReadwiseArticle>& outArticles, int limit) {
  lastHttpCode = 0;
  outArticles.clear();

  if (SETTINGS.articleModuleToken[0] == '\0') return NO_TOKEN;
  if (insufficientHeap()) return LOW_MEMORY;

  char url[128];
  snprintf(url, sizeof(url), "https://readwise.io/api/v3/list/?limit=%d", limit);

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("READWISE", "Bad URL: %s", url);
    return NETWORK_ERROR;
  }
  applyAuthHeader(http);

  outArticles.reserve(static_cast<size_t>(limit));

  // Parses the "results" array directly from the byte stream as it arrives
  // (see ReadwiseListExtractor's header for why): buffering the whole body
  // into one std::string, then handing it to ArduinoJson, needed a single
  // contiguous allocation as large as the response -- confirmed on hardware
  // that this device's typical post-WiFi-connect heap can have plenty of
  // total free bytes but no single contiguous block big enough, and no
  // amount of growth-step tuning fixes a problem that's fundamentally about
  // needing one large block at all.
  ReadwiseListExtractor extractor(outArticles);
  const int httpCode = http.GET([&extractor](const uint8_t* data, size_t len) {
    extractor.feed(reinterpret_cast<const char*>(data), len);
    return !extractor.done();
  });
  http.end();
  lastHttpCode = httpCode;

  if (extractor.error()) {
    LOG_ERR("READWISE", "List response malformed or exceeded the parser's runaway-response cap");
    return SERVER_ERROR;
  }
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401 || httpCode == 403) return AUTH_FAILED;
  if (httpCode < 200 || httpCode >= 300) return SERVER_ERROR;
  if (!extractor.done()) {
    LOG_ERR("READWISE", "List response's \"results\" array never closed");
    return SERVER_ERROR;
  }

  return OK;
}

ReadwiseClient::Error ReadwiseClient::archive(const std::string& documentId) {
  lastHttpCode = 0;

  if (SETTINGS.articleModuleToken[0] == '\0') return NO_TOKEN;
  if (insufficientHeap()) return LOW_MEMORY;

  const std::string url = "https://readwise.io/api/v3/update/" + documentId + "/";

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("READWISE", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeader(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("PATCH", std::string("{\"location\":\"archive\"}"));
  http.end();
  lastHttpCode = httpCode;

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401 || httpCode == 403) return AUTH_FAILED;
  if (httpCode < 200 || httpCode >= 300) return SERVER_ERROR;
  return OK;
}

ReadwiseClient::Error ReadwiseClient::fetchHtmlContent(const std::string& documentId, std::string& outHtml) {
  lastHttpCode = 0;
  outHtml.clear();

  if (SETTINGS.articleModuleToken[0] == '\0') return NO_TOKEN;
  if (insufficientHeap()) return LOW_MEMORY;

  char url[192];
  snprintf(url, sizeof(url), "https://readwise.io/api/v3/list/?id=%s&withHtmlContent=true", documentId.c_str());

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("READWISE", "Bad URL: %s", url);
    return NETWORK_ERROR;
  }
  applyAuthHeader(http);

  // Scans the (still JSON-escaped) response as it arrives for the
  // "html_content" field and unescapes it straight into outHtml, so the raw
  // body — which can be far larger than any other field this client
  // handles — never needs to be buffered whole. Aborting the transfer once
  // the value is captured skips whatever JSON follows it in the response.
  //
  // The extractor's internal std::string still grows unbounded as chunks
  // arrive, though: with -fno-exceptions, a failed allocation calls abort()
  // instead of throwing (see firmware/CLAUDE.md's `new` rule — it applies to
  // STL container growth too, not just explicit `new`). Both caps below are
  // checked every chunk so a huge or pathological article degrades to
  // LOW_MEMORY instead of crashing the device.
  bool tooLarge = false;
  ReadwiseHtmlExtractor extractor;
  const int httpCode = http.GET([&extractor, &tooLarge](const uint8_t* data, size_t len) {
    extractor.feed(reinterpret_cast<const char*>(data), len);
    if (extractor.done()) return false;
    const auto heap = HalMemory::getDefaultHeap();
    if (extractor.size() > MAX_ARTICLE_HTML_BYTES || heap.freeBytes < MIN_FREE_DURING_EXTRACTION ||
        heap.largestBlockBytes < MIN_BLOCK_DURING_EXTRACTION) {
      tooLarge = true;
      return false;
    }
    return true;
  });
  http.end();
  lastHttpCode = httpCode;

  if (tooLarge) {
    LOG_ERR("READWISE", "Article too large or low memory while fetching id %s (%zu bytes captured)", documentId.c_str(),
            extractor.size());
    return LOW_MEMORY;
  }
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401 || httpCode == 403) return AUTH_FAILED;
  if (httpCode < 200 || httpCode >= 300) return SERVER_ERROR;
  if (!extractor.done()) {
    LOG_ERR("READWISE", "html_content missing from response for id %s", documentId.c_str());
    return SERVER_ERROR;
  }

  outHtml = extractor.takeHtml();
  return OK;
}
