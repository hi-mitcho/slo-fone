#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "network/ReadwiseClient.h"

// SD-backed offline cache for the Article Module (see this codebase's PRD.md,
// Article Module section): persists the full synced Readwise article list so
// the list is always browsable without WiFi, plus full-text HTML for the
// newest few unarchived articles so those specific reads work with no
// network at all. Modeled on BookMetadataCache's versioned-binary-file
// pattern rather than CrossPointSettings' JSON store -- a 50-article list
// would spike JSON heap usage during (de)serialization, which is exactly why
// the EPUB cache doesn't use PersistableStore for book metadata either.
//
// Layout under /.crosspoint/articles/:
//   index.bin       - full ReadwiseArticle list from the last successful sync
//   text/<id>.bin   - full-text HTML for a downloaded article, id-named
//
// Eviction: pruneArchived() is the only removal path -- a cached article's
// text is dropped once Readwise reports it archived, never to make room for
// a newer one. Callers cap how many new downloads they attempt per sync
// (see ArticleModuleActivity), this class just stores/serves/prunes.
namespace ArticleOfflineCache {

// Bumped when index.bin's or a text/<id>.bin's binary layout changes; a
// version mismatch on read is treated as "no cache" rather than an error.
inline constexpr uint8_t CACHE_VERSION = 1;

// Soft target for how many articles' full text stay downloaded at once.
// ArticleModuleActivity's sync loop stops adding new downloads once this
// many are cached; it does not evict existing ones to enforce it.
inline constexpr uint8_t MAX_CACHED_TEXT_COUNT = 5;

// Persists the full synced list. Overwrites any existing index.bin.
bool saveIndex(const std::vector<ReadwiseArticle>& articles);

// Loads the last-persisted list into outArticles. Returns false (and leaves
// outArticles untouched) if no cache exists or it fails a version check.
bool loadIndex(std::vector<ReadwiseArticle>& outArticles);

bool isTextCached(const std::string& id);

// Returns false if id has no cached text (outHtml is left untouched).
bool loadText(const std::string& id, std::string& outHtml);

bool saveText(const std::string& id, const std::string& html);

size_t cachedTextCount();

// Deletes cached text for any id not present in stillUnarchived -- i.e.
// articles archived (locally or elsewhere) since the text was downloaded.
void pruneArchived(const std::vector<ReadwiseArticle>& stillUnarchived);

}  // namespace ArticleOfflineCache
