#pragma once
#include <string>
#include <vector>

// Summary fields for one Readwise Reader document, as returned by the list
// endpoint. Deliberately excludes html_content: SLO-6's first slice syncs and
// archives articles but does not render full content yet.
struct ReadwiseArticle {
  std::string id;
  std::string title;
  std::string author;
  std::string summary;
  std::string location;  // "new" | "later" | "shortlist" | "archive" | "feed"
  int wordCount = 0;
};

/**
 * HTTP client for the Readwise Reader API (SLO-6).
 *
 * Base URL: https://readwise.io/api/v3/
 *
 * API Endpoints:
 *   GET   /list/          - List documents (used here: no location filter)
 *   PATCH /update/:id/    - Update a document (used here: location=archive)
 *
 * Authentication: static per-account token, "Authorization: Token <token>".
 * No OAuth flow (see PRD.md's read-later service decision).
 */
class ReadwiseClient {
 public:
  enum Error { OK, NO_TOKEN, LOW_MEMORY, NETWORK_ERROR, AUTH_FAILED, SERVER_ERROR };

  // Fetches unarchived documents across all locations (new/later/shortlist) —
  // freshly-saved items land in "new" by default and many accounts never
  // manually triage into "later", so filtering to just "later" hid them.
  // "archive" and "feed" (RSS) are excluded client-side. Manual/on-demand
  // only (SLO-6) — no incremental updatedAfter tracking in this first slice.
  //
  // Default capped at 20, not Readwise's up-to-100 max: this device's
  // typical post-WiFi-connect free heap (~75KB, after whatever Home/EPUB
  // rendering left resident) isn't reliably enough to buffer and parse a
  // 50+ article response — confirmed on hardware, where listUnarchived()'s
  // own heap guards (see ReadwiseClient.cpp) correctly refused to grow the
  // response buffer past ~17KB rather than risk the fragmented-heap abort()
  // that used to happen here. The offline article cache
  // (ArticleOfflineCache::MAX_CACHED_TEXT_COUNT) only auto-downloads full
  // text for the newest 5 anyway, so a long tail beyond ~20 buys little.
  static Error listUnarchived(std::vector<ReadwiseArticle>& outArticles, int limit = 20);

  // Marks a document archived.
  static Error archive(const std::string& documentId);

  // Fetches the full article body as raw HTML (Readwise/Reader's
  // Mozilla-Readability output — not guaranteed well-formed XHTML; callers
  // must normalize before handing it to an XML-based parser). Streams the
  // response through ReadwiseHtmlExtractor rather than buffering the whole
  // JSON body, so this is safe to call regardless of article length.
  static Error fetchHtmlContent(const std::string& documentId, std::string& outHtml);

  static int lastHttpCode;
};
