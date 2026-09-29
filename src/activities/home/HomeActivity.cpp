#include "HomeActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/weather/WeatherModuleActivity.h"
#include "components/UITheme.h"
#include "components/icons/battery_0.h"
#include "components/icons/battery_100.h"
#include "components/icons/battery_25.h"
#include "components/icons/battery_50.h"
#include "components/icons/battery_75.h"
#include "components/icons/battery_charging.h"
#include "components/icons/blocks.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/settings2.h"
#include "components/icons/slofone_logo.h"
#include "components/icons/transfer.h"
#include "components/icons/weather_clear.h"
#include "components/icons/weather_cloudy.h"
#include "components/icons/weather_drizzle.h"
#include "components/icons/weather_fog.h"
#include "components/icons/weather_rain.h"
#include "components/icons/weather_rainshowers.h"
#include "components/icons/weather_snow.h"
#include "components/icons/weather_snowshowers.h"
#include "components/icons/weather_thunderstorm.h"
#include "fontIds.h"

namespace {
constexpr int QUICK_LINK_ICON_SIZE = 32;
constexpr int QUICK_LINK_ROW_HEIGHT = 56;

// SLO-19: Home has no title, so the shared header band (BaseTheme::drawHeader)
// rendered nothing but an all-but-invisible battery glyph. Home draws its own
// slim status row instead, reclaiming the rest of that vertical space for the
// card grid.
constexpr int STATUS_BAR_HEIGHT = 24;
constexpr int BATTERY_ICON_WIDTH = 35;
constexpr int BATTERY_ICON_HEIGHT = 16;
constexpr int BATTERY_CHARGING_ICON_WIDTH = 29;

// On-screen size of the SLOFONE wordmark bitmap (src/components/icons/slofone_logo.h),
// generated to span the full left-column width (COL_A_WIDTH_RATIO * 480 = 180)
// at the source asset's ~363:223 aspect ratio.
constexpr int LOGO_ICON_WIDTH = 180;
constexpr int LOGO_ICON_HEIGHT = 111;

struct BatteryIcon {
  const uint8_t* bitmap;
  int width;
};

BatteryIcon batteryIconFor(uint16_t percentage, bool charging) {
  if (charging) return {Battery_chargingIcon, BATTERY_CHARGING_ICON_WIDTH};
  if (percentage <= 10) return {Battery_0Icon, BATTERY_ICON_WIDTH};
  if (percentage <= 35) return {Battery_25Icon, BATTERY_ICON_WIDTH};
  if (percentage <= 60) return {Battery_50Icon, BATTERY_ICON_WIDTH};
  if (percentage <= 85) return {Battery_75Icon, BATTERY_ICON_WIDTH};
  return {Battery_100Icon, BATTERY_ICON_WIDTH};
}

// Fractions of the available content area, derived from the SLO-7 mockup
// (480x800 portrait): a narrower left column of stacked info cards next to a
// wider right column carrying the Current Read cover and Next Reader
// Articles cards. Expressed as ratios (not fixed pixels) so the grid scales
// with actual screen dimensions instead of assuming 480x800.
constexpr float SIDE_MARGIN_RATIO = 16.0f / 480.0f;
constexpr float GUTTER_RATIO = 16.0f / 480.0f;
constexpr float COL_A_WIDTH_RATIO = 180.0f / 480.0f;
// Narrower than a plain 480-16-16-180-16=252 split would give: leaves extra
// gutter on the right so the side-button arrow glyphs (drawSideButtonArrows)
// have room to sit clear of the screen edge without crowding this column.
constexpr float COL_B_WIDTH_RATIO = 244.0f / 480.0f;
constexpr float ROW_GAP_RATIO = 12.0f / 686.0f;

// Left column: logo, weather, top-3 reminders, decorative strip.
constexpr float LOGO_HEIGHT_RATIO = 110.0f / 686.0f;
constexpr float WEATHER_HEIGHT_RATIO = 149.0f / 686.0f;
constexpr float REMINDERS_HEIGHT_RATIO = 324.0f / 686.0f;
// Decorative strip takes whatever remains after the three ratios above.

// Right column: Current Read cover, Next Reader Articles.
constexpr float CURRENT_READ_HEIGHT_RATIO = 337.0f / 686.0f;
// Articles card takes whatever remains after the gap.

// SLO-11: one icon per condition bucket from WeatherModuleActivity's shared
// conditionLabel(); STR_WEATHER_COND_CLOUDY also covers STR_WEATHER_COND_UNKNOWN,
// same fallback conditionLabel() itself uses for out-of-range WMO codes.
const uint8_t* weatherConditionIcon(StrId label) {
  switch (label) {
    case StrId::STR_WEATHER_COND_CLEAR:
      return Weather_clearIcon;
    case StrId::STR_WEATHER_COND_FOG:
      return Weather_fogIcon;
    case StrId::STR_WEATHER_COND_DRIZZLE:
      return Weather_drizzleIcon;
    case StrId::STR_WEATHER_COND_RAIN:
      return Weather_rainIcon;
    case StrId::STR_WEATHER_COND_RAIN_SHOWERS:
      return Weather_rainshowersIcon;
    case StrId::STR_WEATHER_COND_SNOW:
      return Weather_snowIcon;
    case StrId::STR_WEATHER_COND_SNOW_SHOWERS:
      return Weather_snowshowersIcon;
    case StrId::STR_WEATHER_COND_THUNDERSTORM:
      return Weather_thunderstormIcon;
    case StrId::STR_WEATHER_COND_CLOUDY:
    case StrId::STR_WEATHER_COND_UNKNOWN:
    default:
      return Weather_cloudyIcon;
  }
}
}  // namespace

int HomeActivity::getSelectableCount() const { return CARD_COUNT + quickLinkCount(hasOpdsServers); }

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  // The card grid shows a single Current Read card, not a carousel.
  loadRecentBooks(1);

  selectorIndex =
      initialMenuItem == HomeMenuItem::NONE ? 0 : CARD_COUNT + menuItemToIndex(initialMenuItem, hasOpdsServers);
  articlesSelectedRow = -1;

  // SLO-21: passive weather refresh, no WiFi-connect UI. Silent no-op unless
  // WiFi already happens to be up and the hourly cooldown has elapsed.
  refreshWeatherIfWifiConnected();

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() { Activity::onExit(); }

void HomeActivity::loop() {
  const int selectableCount = getSelectableCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    switch (selectorIndex) {
      case 0:
        if (!recentBooks.empty()) {
          onSelectBook(recentBooks[0].path);
        }
        return;
      case 1:
        if (articlesSelectedRow >= 0 && articlesSelectedRow < static_cast<int>(SETTINGS.articlesCachedTitleCount)) {
          activityManager.goToArticleModule(SETTINGS.articlesIds[articlesSelectedRow]);
        } else {
          activityManager.goToArticleModule();
        }
        return;
      case WEATHER_CARD_INDEX:
        activityManager.goToWeatherModule();
        return;
      default:
        break;
    }
    const int quickLinkIndex = selectorIndex - CARD_COUNT;
    switch (indexToMenuItem(quickLinkIndex, hasOpdsServers)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  // Moving "next" while the Articles card is highlighted drills into its
  // cached title rows one at a time before advancing to the next card, so
  // the selector can reach individual articles instead of skipping past the
  // whole tile. Symmetric with movePrevious below.
  auto moveNext = [this, selectableCount] {
    if (selectorIndex == ARTICLES_CARD_INDEX &&
        articlesSelectedRow + 1 < static_cast<int>(SETTINGS.articlesCachedTitleCount)) {
      articlesSelectedRow++;
      requestUpdate();
      return;
    }
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, selectableCount);
    articlesSelectedRow = -1;
    requestUpdate();
  };

  auto movePrevious = [this, selectableCount] {
    if (selectorIndex == ARTICLES_CARD_INDEX && articlesSelectedRow > -1) {
      articlesSelectedRow--;
      requestUpdate();
      return;
    }
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, selectableCount);
    // Arriving at the Articles card from below starts at its last row, so
    // continuing to move backward walks the rows before leaving the card.
    articlesSelectedRow = (selectorIndex == ARTICLES_CARD_INDEX && SETTINGS.articlesCachedTitleCount > 0)
                              ? static_cast<int>(SETTINGS.articlesCachedTitleCount) - 1
                              : -1;
    requestUpdate();
  };

  buttonNavigator.onNext(moveNext);
  buttonNavigator.onPrevious(movePrevious);

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    moveNext();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    movePrevious();
    return;
  }

  // Back is otherwise unused on the home screen: open the most recently read
  // book directly (recentBooks is pruned of files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  // Card taps: rects recomputed here to match render()'s geometry (kept
  // local since it's specific to this fixed 2-column layout).
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int sideMargin = static_cast<int>(pageWidth * SIDE_MARGIN_RATIO);
  const int gutter = static_cast<int>(pageWidth * GUTTER_RATIO);
  const int colAWidth = static_cast<int>(pageWidth * COL_A_WIDTH_RATIO);
  const int colBWidth = static_cast<int>(pageWidth * COL_B_WIDTH_RATIO);
  const int colBX = sideMargin + colAWidth + gutter;

  const int contentTop = metrics.topPadding + STATUS_BAR_HEIGHT + metrics.verticalSpacing;
  const int quickLinkAreaHeight = QUICK_LINK_ROW_HEIGHT + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - quickLinkAreaHeight;
  const int contentHeight = contentBottom - contentTop;
  const int rowGap = static_cast<int>(contentHeight * ROW_GAP_RATIO);

  const int currentReadHeight = static_cast<int>(contentHeight * CURRENT_READ_HEIGHT_RATIO);
  const Rect articlesRect{colBX, contentTop + currentReadHeight + rowGap, colBWidth,
                          contentBottom - (contentTop + currentReadHeight + rowGap)};

  // Matches render()'s left-column stacking (logo, then weather) so the tap
  // rect lines up with the drawn card.
  const int logoHeight = static_cast<int>(contentHeight * LOGO_HEIGHT_RATIO);
  const int weatherHeight = static_cast<int>(contentHeight * WEATHER_HEIGHT_RATIO);
  const Rect weatherRect{sideMargin, contentTop + logoHeight + rowGap, colAWidth, weatherHeight};

  int tapX = 0;
  int tapY = 0;
  const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
  const auto inRect = [](const Rect& r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
  };

  int touchedCard = -1;
  if (tapped && inRect(Rect{colBX, contentTop, colBWidth, currentReadHeight}, tapX, tapY)) {
    touchedCard = 0;
  } else if (tapped && inRect(articlesRect, tapX, tapY)) {
    // A tap on a specific cached title jumps straight to that article;
    // anywhere else on the card opens the list, same as button Select.
    const int titleIndex = articleTitleRowIndexAt(articlesRect, tapX, tapY);
    if (titleIndex >= 0) {
      selectorIndex = ARTICLES_CARD_INDEX;
      activityManager.goToArticleModule(SETTINGS.articlesIds[titleIndex]);
      return;
    }
    touchedCard = ARTICLES_CARD_INDEX;
  } else if (tapped && inRect(weatherRect, tapX, tapY)) {
    touchedCard = WEATHER_CARD_INDEX;
  }
  if (touchedCard != -1) {
    selectorIndex = touchedCard;
    articlesSelectedRow = -1;  // a tap always targets the whole card, not a button-nav row highlight
    activateSelection();
    return;
  }

  const int quickLinkTop = contentBottom + metrics.verticalSpacing;
  int quickLinkCol = -1;
  const int quickLinkButtonWidth = (pageWidth - 2 * sideMargin) / quickLinkCount(hasOpdsServers);
  const auto quickLinkTouch =
      mappedInput.colTouch(quickLinkCol, sideMargin, quickLinkButtonWidth, quickLinkCount(hasOpdsServers), quickLinkTop,
                           quickLinkTop + QUICK_LINK_ROW_HEIGHT, quickLinkButtonWidth);
  if (quickLinkTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex = CARD_COUNT + quickLinkCol;
    if (quickLinkTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

int HomeActivity::drawWrappedTitle(const int x, const int y, const int maxWidth, const char* title) const {
  const auto lines = renderer.wrappedText(RESPONDER_18_FONT_ID, title, maxWidth, 2, EpdFontFamily::BOLD);
  int lineY = y;
  for (const auto& line : lines) {
    renderer.drawText(RESPONDER_18_FONT_ID, x, lineY, line.c_str(), true, EpdFontFamily::BOLD);
    lineY += renderer.getLineHeight(RESPONDER_18_FONT_ID);
  }
  return lineY;
}

void HomeActivity::drawLogoCard(const Rect& rect) const {
  // No border: the wordmark is a masthead, not a card among equals, and now
  // sits in the space the shared header band used to leave empty (SLO-19).
  const int iconX = rect.x + (rect.width - LOGO_ICON_WIDTH) / 2;
  const int iconY = rect.y + (rect.height - LOGO_ICON_HEIGHT) / 2;
  renderer.drawIcon(Slofone_logoIcon, iconX, iconY, LOGO_ICON_WIDTH, LOGO_ICON_HEIGHT);
}

void HomeActivity::drawStatusBar(const Rect& rect) const {
  const bool showPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const bool charging = gpio.isUsbConnected();
  const uint16_t percentage = std::min<uint16_t>(powerManager.getBatteryPercentage(), 100);
  const BatteryIcon icon = batteryIconFor(percentage, charging);

  char percentText[8] = {0};
  int percentWidth = 0;
  if (showPercentage) {
    snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned>(percentage));
    percentWidth = renderer.getTextWidth(UI_10_FONT_ID, percentText);
  }

  constexpr int iconTextGap = 6;
  const int groupWidth = icon.width + (showPercentage ? iconTextGap + percentWidth : 0);
  const int iconX = rect.x + rect.width - groupWidth;
  const int iconY = rect.y + (rect.height - BATTERY_ICON_HEIGHT) / 2;
  renderer.drawIcon(icon.bitmap, iconX, iconY, icon.width, BATTERY_ICON_HEIGHT);

  if (showPercentage) {
    const int textX = iconX + icon.width + iconTextGap;
    const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, percentText, true);
  }
}

void HomeActivity::drawWeatherCard(const Rect& rect, bool selected) const {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, selected ? 2 : 1, 8, true);

  char tempLabel[8] = "--°";
  if (SETTINGS.weatherLastFetchUnix != 0) {
    snprintf(tempLabel, sizeof(tempLabel), "%d°", SETTINGS.weatherLastTempF);
  }
  const int textWidth = renderer.getTextWidth(RESPONDER_18_FONT_ID, tempLabel, EpdFontFamily::BOLD);

  // Per-condition icon, laid out side by side with the temperature as one
  // horizontally centered group. drawIcon has no scaling path — size must
  // match the 32x32 the icons were generated at (matches QUICK_LINK_ICON_SIZE),
  // or it misreads the bitmap's row stride.
  constexpr int iconSize = QUICK_LINK_ICON_SIZE;
  constexpr int iconTextGap = 10;
  const int groupWidth = iconSize + iconTextGap + textWidth;
  const int groupX = rect.x + (rect.width - groupWidth) / 2;
  const int groupY = rect.y + rect.height / 2;

  const uint8_t* icon = weatherConditionIcon(conditionLabel(SETTINGS.weatherLastConditionCode));
  renderer.drawIcon(icon, groupX, groupY - iconSize / 2, iconSize);

  const int textX = groupX + iconSize + iconTextGap;
  const int textY = groupY - renderer.getLineHeight(RESPONDER_18_FONT_ID) / 2;
  renderer.drawText(RESPONDER_18_FONT_ID, textX, textY, tempLabel, true, EpdFontFamily::BOLD);
}

void HomeActivity::drawRemindersCard(const Rect& rect) const {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, 8, true);

  constexpr int padding = 14;
  int textY =
      drawWrappedTitle(rect.x + padding, rect.y + padding, rect.width - padding * 2, tr(STR_HOME_REMINDERS_TITLE));
  textY += padding;

  const auto lines =
      renderer.wrappedText(UI_10_FONT_ID, tr(STR_HOME_REMINDERS_PLACEHOLDER), rect.width - padding * 2, 4);
  for (const auto& line : lines) {
    renderer.drawText(UI_10_FONT_ID, rect.x + padding, textY, line.c_str());
    textY += renderer.getLineHeight(UI_10_FONT_ID);
  }
}

void HomeActivity::drawDecorativeStrip(const Rect& rect) const {
  const int midY = rect.y + rect.height / 2;
  const int amplitude = rect.height / 3;
  const int waveLength = std::max(20, rect.width / 6);
  int x = rect.x;
  int prevX = x;
  int prevY = midY;
  bool up = true;
  while (x < rect.x + rect.width) {
    x = std::min(x + waveLength / 2, rect.x + rect.width);
    const int y = midY + (up ? -amplitude : amplitude);
    renderer.drawLine(prevX, prevY, x, y, 3, true);
    prevX = x;
    prevY = y;
    up = !up;
  }
}

void HomeActivity::drawArticlesCard(const Rect& rect, bool selected, int selectedRow) const {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, selected ? 2 : 1, 8, true);

  constexpr int padding = 16;
  int textY =
      drawWrappedTitle(rect.x + padding, rect.y + padding, rect.width - padding * 2, tr(STR_HOME_ARTICLES_TITLE));
  textY += padding;

  // Never synced or nothing unarchived: fall back to the module's own empty-state copy.
  if (SETTINGS.articlesCachedTitleCount == 0) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_ARTICLE_EMPTY), rect.width - padding * 2, 4);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, rect.x + padding, textY, line.c_str());
      textY += renderer.getLineHeight(UI_10_FONT_ID);
    }
    return;
  }

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (uint8_t i = 0; i < SETTINGS.articlesCachedTitleCount; i++) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, SETTINGS.articlesTitles[i], rect.width - padding * 2, 2);
    const int rowHeight = static_cast<int>(lines.size()) * lineHeight;
    if (static_cast<int>(i) == selectedRow) {
      renderer.fillRoundedRect(rect.x + 6, textY - 3, rect.width - 12, rowHeight + 6, 6, Color::LightGray);
    }
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, rect.x + padding, textY, line.c_str());
      textY += lineHeight;
    }
    textY += ARTICLE_ROW_GAP;
  }
}

// Replicates drawArticlesCard's title-block and row geometry without
// drawing, so loop() can hit-test a tap against a specific cached title.
// Returns the cached-title index at (x, y), or -1 if the tap isn't on a row.
int HomeActivity::articleTitleRowIndexAt(const Rect& rect, int x, int y) const {
  if (SETTINGS.articlesCachedTitleCount == 0) return -1;

  constexpr int padding = 16;
  const auto titleLines = renderer.wrappedText(RESPONDER_18_FONT_ID, tr(STR_HOME_ARTICLES_TITLE),
                                               rect.width - padding * 2, 2, EpdFontFamily::BOLD);
  int rowTop =
      rect.y + padding + static_cast<int>(titleLines.size()) * renderer.getLineHeight(RESPONDER_18_FONT_ID) + padding;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  for (uint8_t i = 0; i < SETTINGS.articlesCachedTitleCount; i++) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, SETTINGS.articlesTitles[i], rect.width - padding * 2, 2);
    const int rowHeight = static_cast<int>(lines.size()) * lineHeight;
    if (y >= rowTop && y < rowTop + rowHeight) return static_cast<int>(i);
    rowTop += rowHeight + ARTICLE_ROW_GAP;
  }
  return -1;
}

void HomeActivity::drawCurrentReadCard(const Rect& rect, bool selected) const {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, selected ? 2 : 1, 8, true);

  constexpr int padding = 16;
  int textY =
      drawWrappedTitle(rect.x + padding, rect.y + padding, rect.width - padding * 2, tr(STR_HOME_CURRENT_READ_TITLE));
  textY += padding;

  if (recentBooks.empty()) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_NO_OPEN_BOOK), rect.width - padding * 2, 2);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, rect.x + padding, textY, line.c_str());
      textY += renderer.getLineHeight(UI_10_FONT_ID);
    }
    return;
  }

  const RecentBook& book = recentBooks[0];
  const Rect coverRect{rect.x + padding, textY, rect.width - padding * 2, rect.y + rect.height - padding - textY};

  if (drawBookCoverThumbnail(book, coverRect)) return;

  // No cover available: synthesize one so the card still reads as a "book"
  // rather than a blank gap — bordered rectangle with the title set larger
  // and in a serif face, standing in for cover art.
  renderer.drawRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height);
  constexpr int titlePadding = 12;
  const auto lines =
      renderer.wrappedText(NOTOSERIF_16_FONT_ID, book.title.c_str(), coverRect.width - titlePadding * 2, 6);
  int lineY = coverRect.y + titlePadding;
  for (const auto& line : lines) {
    const int lineWidth = renderer.getTextWidth(NOTOSERIF_16_FONT_ID, line.c_str());
    renderer.drawText(NOTOSERIF_16_FONT_ID, coverRect.x + (coverRect.width - lineWidth) / 2, lineY, line.c_str());
    lineY += renderer.getLineHeight(NOTOSERIF_16_FONT_ID);
  }
}

bool HomeActivity::drawBookCoverThumbnail(const RecentBook& book, const Rect& maxRect) const {
  if (book.path.empty() || maxRect.width <= 0 || maxRect.height <= 0) return false;

  // Recent-book cover generation is otherwise only wired up for the sleep
  // screen (SleepActivity), keyed off the single "last opened" book. Mirror
  // its reopen-and-generate-if-missing pattern here rather than relying on
  // RecentBook::coverBmpPath, whose templated thumb_[HEIGHT].bmp pipeline
  // (Epub::generateThumbBmp) has no caller anywhere and is never populated.
  std::string coverBmpPath;
  if (FsHelpers::hasXtcExtension(book.path)) {
    Xtc xtc(book.path, "/.crosspoint");
    if (!xtc.load() || !xtc.generateCoverBmp()) return false;
    coverBmpPath = xtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(book.path)) {
    Txt txt(book.path, "/.crosspoint");
    if (!txt.load() || !txt.generateCoverBmp()) return false;
    coverBmpPath = txt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    if (!epub.load(true, true) || !epub.generateCoverBmp()) return false;
    coverBmpPath = epub.getCoverBmpPath();
  } else {
    return false;
  }
  if (coverBmpPath.empty()) return false;

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverBmpPath, file)) return false;

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;

  const int imgWidth = bitmap.getWidth();
  const int imgHeight = bitmap.getHeight();
  if (imgWidth <= 0 || imgHeight <= 0) return false;

  // Aspect-fit within maxRect, only shrinking (never upscaling a small
  // cover), then center the result so a non-matching aspect ratio doesn't
  // hug the top-left corner.
  const float scale = std::min(
      1.0f, std::min(static_cast<float>(maxRect.width) / imgWidth, static_cast<float>(maxRect.height) / imgHeight));
  const int renderWidth = std::max(1, static_cast<int>(imgWidth * scale));
  const int renderHeight = std::max(1, static_cast<int>(imgHeight * scale));
  const int renderX = maxRect.x + (maxRect.width - renderWidth) / 2;
  const int renderY = maxRect.y + (maxRect.height - renderHeight) / 2;

  if (!renderer.drawBitmap(bitmap, renderX, renderY, renderWidth, renderHeight)) return false;

  renderer.drawRect(renderX, renderY, renderWidth, renderHeight);
  return true;
}

void HomeActivity::drawQuickLinkStrip(const Rect& rect, int selectedIndex) const {
  const int count = quickLinkCount(hasOpdsServers);
  const int buttonWidth = rect.width / count;

  std::vector<UIIcon> icons = {Folder, Library};
  if (hasOpdsServers) icons.push_back(Blocks);
  icons.push_back(Transfer);
  icons.push_back(Settings);

  for (int i = 0; i < count; ++i) {
    const int buttonX = rect.x + i * buttonWidth;
    const bool selected = selectedIndex == i;
    if (selected) {
      renderer.fillRoundedRect(buttonX + 4, rect.y, buttonWidth - 8, rect.height, 8, Color::LightGray);
    }

    const uint8_t* iconBitmap = nullptr;
    switch (icons[i]) {
      case UIIcon::Folder:
        iconBitmap = FolderIcon;
        break;
      case UIIcon::Library:
        iconBitmap = LibraryIcon;
        break;
      case UIIcon::Blocks:
        iconBitmap = BlocksIcon;
        break;
      case UIIcon::Transfer:
        iconBitmap = TransferIcon;
        break;
      case UIIcon::Settings:
        iconBitmap = Settings2Icon;
        break;
      default:
        break;
    }
    if (iconBitmap == nullptr) continue;

    const int iconX = buttonX + (buttonWidth - QUICK_LINK_ICON_SIZE) / 2;
    const int iconY = rect.y + (rect.height - QUICK_LINK_ICON_SIZE) / 2;
    renderer.drawIcon(iconBitmap, iconX, iconY, QUICK_LINK_ICON_SIZE);
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int sideMargin = static_cast<int>(pageWidth * SIDE_MARGIN_RATIO);
  const int gutter = static_cast<int>(pageWidth * GUTTER_RATIO);
  const int colAWidth = static_cast<int>(pageWidth * COL_A_WIDTH_RATIO);
  const int colBWidth = static_cast<int>(pageWidth * COL_B_WIDTH_RATIO);
  const int colBX = sideMargin + colAWidth + gutter;

  // SLO-19: Home has no title, so it replaces the shared header band with its
  // own slim status row instead of GUI.drawHeader, reclaiming the rest of
  // that vertical space for the card grid (see STATUS_BAR_HEIGHT above).
  drawStatusBar(Rect{sideMargin, metrics.topPadding, pageWidth - 2 * sideMargin, STATUS_BAR_HEIGHT});

  const int contentTop = metrics.topPadding + STATUS_BAR_HEIGHT + metrics.verticalSpacing;
  const int quickLinkAreaHeight = QUICK_LINK_ROW_HEIGHT + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - quickLinkAreaHeight;
  const int contentHeight = contentBottom - contentTop;
  const int rowGap = static_cast<int>(contentHeight * ROW_GAP_RATIO);

  // --- Left column: logo, weather, reminders, decorative strip ---
  int leftY = contentTop;
  const int logoHeight = static_cast<int>(contentHeight * LOGO_HEIGHT_RATIO);
  drawLogoCard(Rect{sideMargin, leftY, colAWidth, logoHeight});
  leftY += logoHeight + rowGap;

  const int weatherHeight = static_cast<int>(contentHeight * WEATHER_HEIGHT_RATIO);
  drawWeatherCard(Rect{sideMargin, leftY, colAWidth, weatherHeight}, selectorIndex == WEATHER_CARD_INDEX);
  leftY += weatherHeight + rowGap;

  const int remindersHeight = static_cast<int>(contentHeight * REMINDERS_HEIGHT_RATIO);
  drawRemindersCard(Rect{sideMargin, leftY, colAWidth, remindersHeight});
  leftY += remindersHeight + rowGap;

  const int decorHeight = contentBottom - leftY;
  drawDecorativeStrip(Rect{sideMargin, leftY, colAWidth, decorHeight});

  // --- Right column: Current Read, Next Reader Articles ---
  const int currentReadHeight = static_cast<int>(contentHeight * CURRENT_READ_HEIGHT_RATIO);
  drawCurrentReadCard(Rect{colBX, contentTop, colBWidth, currentReadHeight}, selectorIndex == 0);

  const int articlesY = contentTop + currentReadHeight + rowGap;
  drawArticlesCard(Rect{colBX, articlesY, colBWidth, contentBottom - articlesY}, selectorIndex == ARTICLES_CARD_INDEX,
                   selectorIndex == ARTICLES_CARD_INDEX ? articlesSelectedRow : -1);

  // --- Quick-link strip below the card grid ---
  const int quickLinkTop = contentBottom + metrics.verticalSpacing;
  drawQuickLinkStrip(Rect{sideMargin, quickLinkTop, pageWidth - 2 * sideMargin, QUICK_LINK_ROW_HEIGHT},
                     selectorIndex - CARD_COUNT);

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonArrows(renderer);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);

  firstRenderDone = true;
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
