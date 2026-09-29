#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  // Selectable cards ahead of the quick-link strip: Current Read, Next Reader
  // Articles, Weather. The Top 3 Reminders card and the logo/decorative slots
  // are display-only, so they're skipped in the selection order.
  static constexpr int CARD_COUNT = 3;
  // Index of the Articles card within the selectable cards (0 = Current Read).
  static constexpr int ARTICLES_CARD_INDEX = 1;
  // Index of the Weather card within the selectable cards.
  static constexpr int WEATHER_CARD_INDEX = 2;
  // Vertical gap between cached title rows in the Articles card. Shared by
  // drawArticlesCard and articleTitleRowIndexAt so their geometry can't drift.
  static constexpr int ARTICLE_ROW_GAP = 10;

  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  // Sub-selection within the Articles card (selectorIndex == 2): -1 means the
  // card itself is highlighted (Select opens the list), 0..cachedCount-1
  // highlights one cached title row (Select opens that article directly).
  // Entering the card from above starts at -1 and Next drills into row 0;
  // entering from below starts at the last row and Previous backs out to -1.
  int articlesSelectedRow = -1;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // Convert HomeMenuItem to a quick-link strip index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::LIBRARY) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert a quick-link strip index back to a HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::LIBRARY;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  static int quickLinkCount(bool hasOpdsUrl) { return hasOpdsUrl ? 5 : 4; }

  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();

  int getSelectableCount() const;
  void loadRecentBooks(int maxBooks);

  // Card grid drawing helpers (render.cpp keeps geometry and drawing together
  // since both are specific to this activity's fixed 2-column layout).
  void drawLogoCard(const Rect& rect) const;
  // Slim replacement for BaseTheme::drawHeader on Home (SLO-19): battery icon
  // + percentage, right-aligned within rect.
  void drawStatusBar(const Rect& rect) const;
  void drawWeatherCard(const Rect& rect, bool selected) const;
  void drawRemindersCard(const Rect& rect) const;
  void drawDecorativeStrip(const Rect& rect) const;
  void drawArticlesCard(const Rect& rect, bool selected, int selectedRow) const;
  // Hit-tests a tap point against the Articles card's cached title rows,
  // matching drawArticlesCard's layout. Returns the tapped title's index, or
  // -1 if the tap wasn't on a row (header, padding, or empty state).
  int articleTitleRowIndexAt(const Rect& rect, int x, int y) const;
  // Current-read card: cover thumbnail (or a synthesized title-card fallback
  // when no cover exists) if a book is in progress, otherwise a "no open
  // book" message. Redraws the cover fresh from SD every render instead of
  // reusing BaseTheme's shared drawRecentBookCover, whose full-framebuffer
  // snapshot/restore caching assumes a full-width tile and stomps this
  // layout's other cards when fed its narrower card rect.
  void drawCurrentReadCard(const Rect& rect, bool selected) const;
  // Generates (if missing) and draws the given book's cover bitmap, aspect-fit
  // and centered within maxRect, with a border around the rendered bounds.
  // Returns false if the book has no cover or the bitmap couldn't be read,
  // in which case the caller should fall back to a synthesized title card.
  bool drawBookCoverThumbnail(const RecentBook& book, const Rect& maxRect) const;
  // Compact icon-only row replacing the full-size vertical button list, so
  // the card grid gets most of the screen. rect spans the whole row; icons
  // are evenly spaced within it.
  void drawQuickLinkStrip(const Rect& rect, int selectedIndex) const;
  // Draws a card title wrapped to at most 2 lines instead of a single
  // unwrapped line, so longer strings (translations, "Continue Reading")
  // don't run past the card's edge. Returns the y position immediately
  // below the title block, for body text drawn under it.
  int drawWrappedTitle(int x, int y, int maxWidth, const char* title) const;

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
