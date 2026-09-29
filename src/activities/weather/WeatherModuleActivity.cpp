#include "WeatherModuleActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <variant>

#include "CrossPointSettings.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

StrId conditionLabel(uint8_t wmoCode) {
  if (wmoCode == 0) return StrId::STR_WEATHER_COND_CLEAR;
  if (wmoCode <= 3) return StrId::STR_WEATHER_COND_CLOUDY;
  if (wmoCode == 45 || wmoCode == 48) return StrId::STR_WEATHER_COND_FOG;
  if (wmoCode >= 51 && wmoCode <= 57) return StrId::STR_WEATHER_COND_DRIZZLE;
  if (wmoCode >= 61 && wmoCode <= 67) return StrId::STR_WEATHER_COND_RAIN;
  if (wmoCode >= 71 && wmoCode <= 77) return StrId::STR_WEATHER_COND_SNOW;
  if (wmoCode >= 80 && wmoCode <= 82) return StrId::STR_WEATHER_COND_RAIN_SHOWERS;
  if (wmoCode >= 85 && wmoCode <= 86) return StrId::STR_WEATHER_COND_SNOW_SHOWERS;
  if (wmoCode >= 95) return StrId::STR_WEATHER_COND_THUNDERSTORM;
  return StrId::STR_WEATHER_COND_UNKNOWN;
}

// Guards against an unsynced RTC reading near 0: treats any epoch before this
// (~2023-11-14) as "clock not synced yet" rather than a stale-but-valid fetch.
constexpr time_t MIN_PLAUSIBLE_EPOCH = 1700000000;
constexpr uint32_t CACHE_FRESH_SECONDS = 3600;  // SLO-8/SLO-21: hourly cooldown

bool weatherCacheIsFresh() {
  const time_t now = time(nullptr);
  const bool clockSynced = now > MIN_PLAUSIBLE_EPOCH;
  return clockSynced && SETTINGS.weatherLastFetchUnix != 0 &&
         (static_cast<uint32_t>(now) - SETTINGS.weatherLastFetchUnix) < CACHE_FRESH_SECONDS;
}

WeatherRefreshOutcome refreshWeatherIfWifiConnected() {
  if (weatherCacheIsFresh()) return WeatherRefreshOutcome::Skipped;

  if (WiFi.status() != WL_CONNECTED) return WeatherRefreshOutcome::NoWifi;

  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("WTHR", "Low heap for fetch (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return WeatherRefreshOutcome::Failed;
  }

  double lat = 0;
  double lon = 0;
  if (sscanf(SETTINGS.weatherLocation, "%lf,%lf", &lat, &lon) != 2) {
    LOG_ERR("WTHR", "Malformed weatherLocation: '%s'", SETTINGS.weatherLocation);
    return WeatherRefreshOutcome::Failed;
  }

  char url[224];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code"
           "&daily=temperature_2m_max,temperature_2m_min&temperature_unit=fahrenheit&timezone=auto&forecast_days=1",
           lat, lon);

  std::string body;
  if (!HttpDownloader::fetchUrl(url, body)) {
    LOG_ERR("WTHR", "Fetch failed");
    return WeatherRefreshOutcome::Failed;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err || !doc["current"]["temperature_2m"].is<float>() || !doc["daily"]["temperature_2m_max"][0].is<float>() ||
      !doc["daily"]["temperature_2m_min"][0].is<float>()) {
    LOG_ERR("WTHR", "Unexpected response shape: %s", err ? err.c_str() : "missing fields");
    return WeatherRefreshOutcome::Failed;
  }

  const time_t now = time(nullptr);
  SETTINGS.weatherLastTempF = static_cast<int16_t>(lroundf(doc["current"]["temperature_2m"].as<float>()));
  SETTINGS.weatherLastHiF = static_cast<int16_t>(lroundf(doc["daily"]["temperature_2m_max"][0].as<float>()));
  SETTINGS.weatherLastLoF = static_cast<int16_t>(lroundf(doc["daily"]["temperature_2m_min"][0].as<float>()));
  SETTINGS.weatherLastConditionCode = static_cast<uint8_t>(doc["current"]["weather_code"] | 0);
  if (now > MIN_PLAUSIBLE_EPOCH) {
    SETTINGS.weatherLastFetchUnix = static_cast<uint32_t>(now);
  }
  SETTINGS.saveToFile();
  return WeatherRefreshOutcome::Success;
}

namespace {

void saveLocation(const char* location) {
  strncpy(SETTINGS.weatherLocation, location, sizeof(SETTINGS.weatherLocation) - 1);
  SETTINGS.weatherLocation[sizeof(SETTINGS.weatherLocation) - 1] = '\0';
  SETTINGS.saveToFile();
}

}  // namespace

void WeatherModuleActivity::onEnter() {
  Activity::onEnter();
  noWifi = false;
  fetchFailed = false;
  locationLookupFailed = false;

  if (SETTINGS.weatherLocation[0] == '\0') {
    promptForLocation();
    return;
  }

  beginRefresh();
}

void WeatherModuleActivity::onExit() {
  Activity::onExit();
  if (shouldTearDownWifiOnExit && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void WeatherModuleActivity::promptForLocation() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_LOCATION_PROMPT),
                                                                 "", 31, InputType::Text),
                         [this](const ActivityResult& result) { onLocationEntered(result); });
}

void WeatherModuleActivity::ensureWifiConnected(std::function<void()> onConnected) {
  if (WiFi.status() == WL_CONNECTED) {
    onConnected();
    return;
  }

  shouldTearDownWifiOnExit = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this, onConnected](const ActivityResult& result) {
                           if (result.isCancelled) {
                             noWifi = true;
                             state = State::SHOWING;
                             requestUpdate();
                             return;
                           }
                           onConnected();
                         });
}

void WeatherModuleActivity::onLocationEntered(const ActivityResult& result) {
  if (result.isCancelled) {
    finish();
    return;
  }

  const auto& kb = std::get<KeyboardResult>(result.data);
  const std::string input = kb.text;
  const bool isZip = input.size() == 5 &&
                     std::all_of(input.begin(), input.end(), [](unsigned char c) { return std::isdigit(c) != 0; });

  if (!isZip) {
    // Fallback: a literal "lat,lon" for exact coordinates.
    saveLocation(input.c_str());
    beginRefresh();
    return;
  }

  ensureWifiConnected([this, input] {
    double lat = 0;
    double lon = 0;
    if (!resolveZipToLatLon(input, lat, lon)) {
      locationLookupFailed = true;
      state = State::SHOWING;
      requestUpdate();
      return;
    }
    char locationBuf[sizeof(CrossPointSettings::weatherLocation)];
    snprintf(locationBuf, sizeof(locationBuf), "%.4f,%.4f", lat, lon);
    saveLocation(locationBuf);
    beginRefresh();
  });
}

bool WeatherModuleActivity::resolveZipToLatLon(const std::string& zip, double& lat, double& lon) {
  char url[64];
  snprintf(url, sizeof(url), "http://api.zippopotam.us/us/%s", zip.c_str());

  std::string body;
  if (!HttpDownloader::fetchUrl(url, body)) {
    LOG_ERR("WTHR", "Zip lookup fetch failed");
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body) || !doc["places"][0]["latitude"].is<const char*>() ||
      !doc["places"][0]["longitude"].is<const char*>()) {
    LOG_ERR("WTHR", "Zip lookup response malformed");
    return false;
  }

  lat = atof(doc["places"][0]["latitude"].as<const char*>());
  lon = atof(doc["places"][0]["longitude"].as<const char*>());
  return true;
}

void WeatherModuleActivity::beginRefresh() {
  if (weatherCacheIsFresh()) {
    state = State::SHOWING;
    requestUpdate();
    return;
  }

  ensureWifiConnected([this] {
    state = State::LOADING;
    requestUpdate();
  });
}

void WeatherModuleActivity::refreshIfNeeded() {
  switch (refreshWeatherIfWifiConnected()) {
    case WeatherRefreshOutcome::NoWifi:
      // Defensive: beginRefresh() only reaches LOADING once WiFi is up, but
      // the connection could still drop in the brief window before this runs.
      LOG_INF("WTHR", "WiFi dropped before fetch could run");
      noWifi = true;
      break;
    case WeatherRefreshOutcome::Failed:
      fetchFailed = true;
      break;
    case WeatherRefreshOutcome::Skipped:
    case WeatherRefreshOutcome::Success:
      break;
  }
}

void WeatherModuleActivity::loop() {
  if (state == State::LOADING) {
    // First-tick: render "Loading..." before the (blocking) network call.
    requestUpdateAndWait();
    refreshIfNeeded();
    state = State::SHOWING;
    requestUpdate();
    return;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void WeatherModuleActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEATHER_MODULE_TITLE));

  const int midY = pageHeight / 2;

  if (state == State::LOADING) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_WEATHER_LOADING));
  } else if (locationLookupFailed) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_WEATHER_LOCATION_LOOKUP_FAILED));
  } else if (SETTINGS.weatherLastFetchUnix == 0) {
    // Never successfully fetched: nothing cached to fall back on.
    renderer.drawCenteredText(UI_12_FONT_ID, midY, noWifi ? tr(STR_WEATHER_NO_WIFI) : tr(STR_WEATHER_FETCH_FAILED));
  } else {
    char tempBuf[8];
    snprintf(tempBuf, sizeof(tempBuf), "%d°", SETTINGS.weatherLastTempF);
    renderer.drawCenteredText(NOTOSANS_18_FONT_ID, midY - 60, tempBuf, true, EpdFontFamily::BOLD);

    renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, I18N.get(conditionLabel(SETTINGS.weatherLastConditionCode)));

    char hiLoBuf[32];
    snprintf(hiLoBuf, sizeof(hiLoBuf), tr(STR_WEATHER_HI_LO), SETTINGS.weatherLastHiF, SETTINGS.weatherLastLoF);
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, hiLoBuf);

    char statusBuf[48];
    if (fetchFailed) {
      snprintf(statusBuf, sizeof(statusBuf), "%s", tr(STR_WEATHER_STALE_FAILED));
    } else if (noWifi) {
      snprintf(statusBuf, sizeof(statusBuf), "%s", tr(STR_WEATHER_STALE_NO_WIFI));
    } else {
      const time_t now = time(nullptr);
      const uint32_t ageSeconds =
          now > MIN_PLAUSIBLE_EPOCH ? static_cast<uint32_t>(now) - SETTINGS.weatherLastFetchUnix : 0;
      if (ageSeconds < 60) {
        snprintf(statusBuf, sizeof(statusBuf), "%s", tr(STR_WEATHER_SYNCED_JUST_NOW));
      } else if (ageSeconds < 3600) {
        snprintf(statusBuf, sizeof(statusBuf), tr(STR_WEATHER_SYNCED_MIN_AGO), ageSeconds / 60);
      } else {
        snprintf(statusBuf, sizeof(statusBuf), tr(STR_WEATHER_SYNCED_HR_AGO), ageSeconds / 3600);
      }
    }
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 40, statusBuf);
  }

  if (state != State::LOADING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
