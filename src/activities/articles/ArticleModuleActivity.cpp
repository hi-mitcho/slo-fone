#include "ArticleModuleActivity.h"

#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <variant>

#include "ArticleDetailActivity.h"
#include "ArticleOfflineCache.h"
#include "ArticleReaderActivity.h"
#include "CrossPointSettings.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/weather/WeatherModuleActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

void ArticleModuleActivity::onEnter() {
  UiListActivity::onEnter();
  noWifi = false;
  syncFailed = false;
  nav.selected = 0;

  if (SETTINGS.articleModuleToken[0] == '\0') {
    promptForToken();
    return;
  }

  const bool haveCache = ArticleOfflineCache::loadIndex(articles);
  if (haveCache) rebuildRowItems();

  const bool pendingTextCached = !pendingArticleId.empty() && ArticleOfflineCache::isTextCached(pendingArticleId);
  // Sync (and the WiFi it needs) is only required when there's no offline
  // list yet, or the caller wants to auto-read a specific article whose text
  // isn't cached — everything else (plain browsing, or a pending article
  // that's already downloaded) is servable straight from SD.
  const bool needsSync = !haveCache || (pendingArticleAutoRead && !pendingArticleId.empty() && !pendingTextCached);
  if (needsSync) {
    beginSync();
    return;
  }

  state = State::LIST;
  requestUpdate();
  openPendingArticleIfPresent();
}

void ArticleModuleActivity::onExit() {
  Activity::onExit();
  if (shouldTearDownWifiOnExit && WiFi.getMode() != WIFI_MODE_NULL) {
    // Piggyback weather's hourly refresh here, not on the WiFi connect path:
    // by now this activity's own Readwise TLS traffic (list sync plus any
    // downloadCachedArticleText() downloads) is done and its buffers are
    // freed, so this doesn't stack concurrent handshakes and risk the
    // heap-fragmentation abort that hit the earlier WifiSelectionActivity
    // hook. Always attempted, even on a download-heavy sync pass:
    // refreshWeatherIfWifiConnected() no-ops on its own cooldown
    // (weatherCacheIsFresh()) and fails gracefully rather than crashing if
    // heap is too tight (HttpDownloader::MIN_TLS_FREE_HEAP/MIN_TLS_MAX_ALLOC
    // preflight) — skipping it here whenever downloads happened would risk
    // missing the hourly window entirely if WiFi doesn't come back up again
    // before the next cooldown check.
    refreshWeatherIfWifiConnected();
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ArticleModuleActivity::promptForToken() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ARTICLE_TOKEN_PROMPT),
                                                                 "", 63, InputType::Password),
                         [this](const ActivityResult& result) { onTokenEntered(result); });
}

void ArticleModuleActivity::onTokenEntered(const ActivityResult& result) {
  if (result.isCancelled) {
    finish();
    return;
  }

  const auto& kb = std::get<KeyboardResult>(result.data);
  strncpy(SETTINGS.articleModuleToken, kb.text.c_str(), sizeof(SETTINGS.articleModuleToken) - 1);
  SETTINGS.articleModuleToken[sizeof(SETTINGS.articleModuleToken) - 1] = '\0';
  SETTINGS.saveToFile();

  beginSync();
}

void ArticleModuleActivity::ensureWifiConnected(std::function<void()> onConnected) {
  if (WiFi.status() == WL_CONNECTED) {
    onConnected();
    return;
  }

  shouldTearDownWifiOnExit = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this, onConnected](const ActivityResult& result) {
                           if (result.isCancelled) {
                             noWifi = true;
                             state = State::LIST;
                             requestUpdate();
                             return;
                           }
                           onConnected();
                         });
}

void ArticleModuleActivity::beginSync() {
  ensureWifiConnected([this] {
    state = State::LOADING;
    requestUpdate();
  });
}

void ArticleModuleActivity::syncArticles() {
  // ensureWifiConnected()'s callback fires as soon as WiFi.status() first
  // reports WL_CONNECTED, which can be momentarily ahead of the connection
  // actually settling (DHCP/DNS) — most visible right after a silent restart
  // (SLO-15's read flow), where there's been no time for anything else to
  // run in between. Give it a brief grace window to catch up before treating
  // a not-yet-connected read as a genuine drop.
  constexpr uint32_t WIFI_SETTLE_TIMEOUT_MS = 1500;
  const uint32_t settleDeadline = millis() + WIFI_SETTLE_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < settleDeadline) {
    delay(50);
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("ARTM", "WiFi dropped before sync could run");
    noWifi = true;
    return;
  }

  const auto err = ReadwiseClient::listUnarchived(articles);
  if (err != ReadwiseClient::OK) {
    LOG_ERR("ARTM", "Sync failed: error %d (http %d)", err, ReadwiseClient::lastHttpCode);
    syncFailed = true;
    return;
  }

  rebuildRowItems();
  cacheSyncResultForHomeScreen();
  ArticleOfflineCache::saveIndex(articles);
  ArticleOfflineCache::pruneArchived(articles);
  downloadCachedArticleText();
}

void ArticleModuleActivity::downloadCachedArticleText() {
  size_t cachedCount = ArticleOfflineCache::cachedTextCount();
  for (const auto& a : articles) {
    if (cachedCount >= ArticleOfflineCache::MAX_CACHED_TEXT_COUNT) break;
    if (ArticleOfflineCache::isTextCached(a.id)) continue;

    // Settle window between sequential TLS handshakes — same insurance as
    // the delay(30) before silentRestart() below, cheap relative to the risk
    // of hammering an already fragmenting heap (see onExit()'s comment).
    delay(50);

    std::string html;
    const auto fetchErr = ReadwiseClient::fetchHtmlContent(a.id, html);
    if (fetchErr != ReadwiseClient::OK) {
      // Stop rather than skip-and-continue: a failure here (especially
      // LOW_MEMORY) means the next attempt is likely to fail the same way,
      // and each attempted handshake costs heap/fragmentation even when it
      // bails out cleanly.
      const auto heap = HalMemory::getDefaultHeap();
      LOG_INF("ARTM", "Stopping article text downloads: fetch failed (err %d), free=%zu largest=%zu", fetchErr,
              heap.freeBytes, heap.largestBlockBytes);
      break;
    }
    if (!ArticleOfflineCache::saveText(a.id, html)) {
      LOG_ERR("ARTM", "Failed to save cached text for article %s", a.id.c_str());
      break;
    }
    cachedCount++;
    const auto heap = HalMemory::getDefaultHeap();
    LOG_INF("ARTM", "Cached article text for %s (%zu/%d, free=%zu largest=%zu)", a.id.c_str(), cachedCount,
            ArticleOfflineCache::MAX_CACHED_TEXT_COUNT, heap.freeBytes, heap.largestBlockBytes);
  }
}

void ArticleModuleActivity::cacheSyncResultForHomeScreen() {
  const uint8_t count =
      std::min(static_cast<uint8_t>(articles.size()), CrossPointSettings::ARTICLES_CACHED_TITLE_COUNT);
  for (uint8_t i = 0; i < count; i++) {
    const char* title = articles[i].title.empty() ? tr(STR_ARTICLE_UNTITLED) : articles[i].title.c_str();
    strncpy(SETTINGS.articlesTitles[i], title, sizeof(SETTINGS.articlesTitles[0]) - 1);
    SETTINGS.articlesTitles[i][sizeof(SETTINGS.articlesTitles[0]) - 1] = '\0';
    strncpy(SETTINGS.articlesIds[i], articles[i].id.c_str(), sizeof(SETTINGS.articlesIds[0]) - 1);
    SETTINGS.articlesIds[i][sizeof(SETTINGS.articlesIds[0]) - 1] = '\0';
  }
  SETTINGS.articlesCachedTitleCount = count;
  SETTINGS.articlesLastSyncUnix = static_cast<uint32_t>(time(nullptr));
  SETTINGS.saveToFile();
}

void ArticleModuleActivity::loop() {
  if (state == State::CONNECTING) {
    // Waiting on ensureWifiConnected()'s callback (see the State comment in
    // the header) — nothing to do yet.
    return;
  }
  if (state == State::LOADING) {
    // First-tick: render "Loading..." before the (blocking) network call.
    requestUpdateAndWait();
    syncArticles();
    state = State::LIST;
    requestUpdate();
    openPendingArticleIfPresent();
    return;
  }

  UiListActivity::loop();
}

void ArticleModuleActivity::openPendingArticleIfPresent() {
  if (pendingArticleId.empty()) return;
  const std::string id = std::move(pendingArticleId);
  pendingArticleId.clear();
  const bool autoRead = pendingArticleAutoRead;
  pendingArticleAutoRead = false;

  const auto it =
      std::find_if(articles.begin(), articles.end(), [&id](const ReadwiseArticle& a) { return a.id == id; });
  if (it == articles.end()) return;

  if (autoRead && ArticleOfflineCache::isTextCached(id)) {
    // No new WiFi/TLS session needed — skip ArticleDetailActivity and the
    // restart-based heap defrag entirely (see ArticleDetailActivity.cpp's
    // Confirm handler for the equivalent, non-restart branch).
    openCachedArticleDirectly(*it);
    return;
  }
  activateIndexWithAutoRead(static_cast<int>(it - articles.begin()), autoRead);
}

void ArticleModuleActivity::openCachedArticleDirectly(const ReadwiseArticle& article) {
  std::string html;
  if (!ArticleOfflineCache::loadText(article.id, html)) {
    // Cache entry vanished between the isTextCached() check and here (e.g.
    // pruned) — fall back to the normal (network-requiring) detail flow.
    const auto it = std::find_if(articles.begin(), articles.end(),
                                 [&article](const ReadwiseArticle& a) { return a.id == article.id; });
    if (it != articles.end()) activateIndexWithAutoRead(static_cast<int>(it - articles.begin()), true);
    return;
  }
  startActivityForResult(std::make_unique<ArticleReaderActivity>(renderer, mappedInput, article.title, std::move(html)),
                         [this](const ActivityResult&) { requestUpdate(); });
}

int ArticleModuleActivity::listCount() const { return state == State::LIST ? static_cast<int>(articles.size()) : 0; }

void ArticleModuleActivity::rebuildRowItems() {
  rowItems_.clear();
  rowItems_.reserve(articles.size());
  for (const auto& a : articles) {
    fui::ListItem item;
    item.label = a.title.empty() ? tr(STR_ARTICLE_UNTITLED) : a.title.c_str();
    if (!a.author.empty()) item.subtitle = a.author.c_str();
    if (ArticleOfflineCache::isTextCached(a.id)) item.icon = listIconFor(UIIcon::Download, 24);
    rowItems_.push_back(item);
  }
}

void ArticleModuleActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state == State::CONNECTING || state == State::LOADING) {
    screen.centeredText(tr(STR_ARTICLE_LOADING), screen.theme().bodyText);
    return;
  }

  if (articles.empty()) {
    const char* msg = noWifi       ? tr(STR_ARTICLE_NO_WIFI)
                      : syncFailed ? tr(STR_ARTICLE_SYNC_FAILED)
                                   : tr(STR_ARTICLE_EMPTY);
    screen.centeredText(msg, screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}

void ArticleModuleActivity::activateIndex(int index) { activateIndexWithAutoRead(index, false); }

void ArticleModuleActivity::activateIndexWithAutoRead(int index, bool autoRead) {
  if (index < 0 || index >= static_cast<int>(articles.size())) return;
  startActivityForResult(std::make_unique<ArticleDetailActivity>(renderer, mappedInput, articles[index], autoRead),
                         [this, index](const ActivityResult& result) { onDetailClosed(index, result); });
}

void ArticleModuleActivity::onDetailClosed(int index, const ActivityResult& result) {
  if (!result.isCancelled) {
    const auto* detail = std::get_if<ArticleDetailResult>(&result.data);
    if (detail && detail->archived && index >= 0 && index < static_cast<int>(articles.size())) {
      articles.erase(articles.begin() + index);
      rebuildRowItems();
      if (nav.selected >= static_cast<int>(articles.size())) {
        nav.selected = std::max(0, static_cast<int>(articles.size()) - 1);
      }
    }
  }
  requestUpdate();
}

const char* ArticleModuleActivity::headerTitle() const { return tr(STR_ARTICLE_MODULE_TITLE); }

bool ArticleModuleActivity::handleCustomInput() {
  if (state != State::LIST) return false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    refreshRequested();
    return true;
  }
  return false;
}

void ArticleModuleActivity::drawFooter() {
  if (state != State::LIST) {
    UiListActivity::drawFooter();
    return;
  }
  // Front Right is repurposed for manual refresh; list scrolling is side
  // Up/Down only app-wide now (see MappedInputManager::mapButton()'s
  // NavNext/NavPrevious case), so there's no footer slot left to reclaim for
  // it. Left stays unlabeled/unused, matching every other list screen.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", tr(STR_ARTICLE_REFRESH));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonArrows(renderer);
}

void ArticleModuleActivity::refreshRequested() { beginSync(); }
