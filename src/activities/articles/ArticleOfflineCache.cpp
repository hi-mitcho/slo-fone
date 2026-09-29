#include "ArticleOfflineCache.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

namespace ArticleOfflineCache {

namespace {
constexpr char kArticlesDir[] = "/.crosspoint/articles";
constexpr char kTextDir[] = "/.crosspoint/articles/text";
constexpr char kIndexPath[] = "/.crosspoint/articles/index.bin";

std::string textPath(const std::string& id) { return std::string(kTextDir) + "/" + id + ".bin"; }

void writeArticle(HalFile& file, const ReadwiseArticle& a) {
  serialization::writeString(file, a.id);
  serialization::writeString(file, a.title);
  serialization::writeString(file, a.author);
  serialization::writeString(file, a.summary);
  serialization::writeString(file, a.location);
  serialization::writePod(file, a.wordCount);
}

ReadwiseArticle readArticle(HalFile& file) {
  ReadwiseArticle a;
  serialization::readString(file, a.id);
  serialization::readString(file, a.title);
  serialization::readString(file, a.author);
  serialization::readString(file, a.summary);
  serialization::readString(file, a.location);
  serialization::readPod(file, a.wordCount);
  return a;
}
}  // namespace

bool saveIndex(const std::vector<ReadwiseArticle>& articles) {
  if (!Storage.ensureDirectoryExists(kArticlesDir)) {
    LOG_ERR("ARTC", "Failed to create %s", kArticlesDir);
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite("ARTC", kIndexPath, file)) {
    LOG_ERR("ARTC", "Failed to open %s for write", kIndexPath);
    return false;
  }

  serialization::writePod(file, CACHE_VERSION);
  const uint16_t count = static_cast<uint16_t>(articles.size());
  serialization::writePod(file, count);
  for (const auto& a : articles) writeArticle(file, a);
  return true;
}

bool loadIndex(std::vector<ReadwiseArticle>& outArticles) {
  HalFile file;
  if (!Storage.openFileForRead("ARTC", kIndexPath, file)) return false;

  uint8_t version = 0;
  serialization::readPod(file, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("ARTC", "index.bin version mismatch: expected %d, got %d", CACHE_VERSION, version);
    return false;
  }

  uint16_t count = 0;
  serialization::readPod(file, count);
  std::vector<ReadwiseArticle> loaded;
  loaded.reserve(count);
  for (uint16_t i = 0; i < count; i++) loaded.push_back(readArticle(file));

  outArticles = std::move(loaded);
  return true;
}

bool isTextCached(const std::string& id) { return Storage.exists(textPath(id).c_str()); }

bool loadText(const std::string& id, std::string& outHtml) {
  HalFile file;
  if (!Storage.openFileForRead("ARTC", textPath(id), file)) return false;

  uint8_t version = 0;
  serialization::readPod(file, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("ARTC", "text cache version mismatch for %s: expected %d, got %d", id.c_str(), CACHE_VERSION, version);
    return false;
  }

  std::string html;
  serialization::readString(file, html);
  outHtml = std::move(html);
  return true;
}

bool saveText(const std::string& id, const std::string& html) {
  if (!Storage.ensureDirectoryExists(kTextDir)) {
    LOG_ERR("ARTC", "Failed to create %s", kTextDir);
    return false;
  }

  const std::string path = textPath(id);
  HalFile file;
  if (!Storage.openFileForWrite("ARTC", path, file)) {
    LOG_ERR("ARTC", "Failed to open %s for write", path.c_str());
    return false;
  }

  serialization::writePod(file, CACHE_VERSION);
  serialization::writeString(file, html);
  return true;
}

size_t cachedTextCount() { return Storage.listFiles(kTextDir, 255).size(); }

void pruneArchived(const std::vector<ReadwiseArticle>& stillUnarchived) {
  const auto cached = Storage.listFiles(kTextDir, 255);
  for (const auto& name : cached) {
    std::string id(name.c_str());
    const auto dot = id.rfind(".bin");
    if (dot != std::string::npos) id = id.substr(0, dot);

    const bool stillPresent =
        std::any_of(stillUnarchived.begin(), stillUnarchived.end(), [&id](const ReadwiseArticle& a) { return a.id == id; });
    if (!stillPresent) {
      LOG_DBG("ARTC", "Pruning archived article text cache: %s", id.c_str());
      Storage.remove(textPath(id).c_str());
    }
  }
}

}  // namespace ArticleOfflineCache
