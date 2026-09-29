#pragma once
#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityResult.h"
#include "activities/UiListActivity.h"
#include "network/ReadwiseClient.h"

// Article Module (SLO-6): syncs the Readwise Reader "later" queue and lists
// it. Selecting a row opens ArticleDetailActivity for metadata + archive;
// full HTML rendering is a separate follow-up (see ArticleDetailActivity).
//
// Offline: the last-synced list persists to SD via ArticleOfflineCache, so
// onEnter() shows it immediately without touching WiFi when a cache exists —
// sync (and the WiFi it requires) only runs on first use or when the user
// hits Refresh (see handleCustomInput()). Full text for the newest few
// unarchived articles is auto-downloaded opportunistically during sync (see
// syncArticles() / downloadCachedArticleText()), so those specific articles
// open and read with no network at all.
//
// WiFi: brings up its own connection via WifiSelectionActivity when needed
// and restarts on exit if it did (see WeatherModuleActivity's header comment
// for the full rationale — Settings > Network never leaves a connection for
// a later Activity to inherit).
class ArticleModuleActivity final : public UiListActivity {
 public:
  // initialArticleId: when set (Home Screen tapped a cached title directly,
  // or resuming a SLO-15 read-triggered restart), jump straight to that
  // article's detail instead of showing the list first — served from the
  // offline index cache when possible, otherwise once sync completes. Falls
  // back to the list if the id isn't found (e.g. archived elsewhere since
  // the cache was written). autoReadPendingArticle additionally starts the
  // full-text read flow immediately instead of showing its summary — only
  // meaningful alongside a non-empty initialArticleId; if that article's
  // text is already offline-cached, this opens it with no network at all.
  explicit ArticleModuleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string initialArticleId = "", bool autoReadPendingArticle = false)
      : UiListActivity("ArticleModule", renderer, mappedInput),
        pendingArticleId(std::move(initialArticleId)),
        pendingArticleAutoRead(autoReadPendingArticle) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleCustomInput() override;
  void drawFooter() override;

 private:
  // CONNECTING (not LOADING) is the initial state: onEnter()'s beginSync()
  // call is synchronous, but WifiSelectionActivity itself only becomes the
  // active activity on a later tick (pushActivity defers the swap). If this
  // defaulted to LOADING, a loop() call landing in that gap would run
  // syncArticles() with WiFi not yet connected — reliably hit when
  // ArticleModuleActivity is the very first activity after boot (SLO-15's
  // post-restart auto-read), where there's no prior warm scheduling slack to
  // mask it. ensureWifiConnected()'s callback (both the already-connected
  // fast path and the WifiSelectionActivity success path) is what actually
  // advances CONNECTING -> LOADING.
  enum class State { CONNECTING, LOADING, LIST };
  State state = State::CONNECTING;
  bool noWifi = false;
  bool syncFailed = false;
  bool shouldTearDownWifiOnExit = false;
  std::vector<ReadwiseArticle> articles;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::string pendingArticleId;
  bool pendingArticleAutoRead = false;

  void promptForToken();
  void onTokenEntered(const ActivityResult& result);
  void ensureWifiConnected(std::function<void()> onConnected);
  void beginSync();
  void syncArticles();
  void cacheSyncResultForHomeScreen();
  void downloadCachedArticleText();
  void rebuildRowItems();
  void openPendingArticleIfPresent();
  void openCachedArticleDirectly(const ReadwiseArticle& article);
  void activateIndexWithAutoRead(int index, bool autoRead);
  void onDetailClosed(int index, const ActivityResult& result);
  void refreshRequested();
};
