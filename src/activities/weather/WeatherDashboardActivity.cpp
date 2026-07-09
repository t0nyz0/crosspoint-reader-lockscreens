#include "WeatherDashboardActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DashboardSleep.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/dashboard/DashboardUI.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
// WMO weather codes (https://open-meteo.com/en/docs) collapsed into broad
// categories for a short label + a simple primitive-drawn icon.
enum class WxCategory { Clear, PartlyCloudy, Cloudy, Fog, Drizzle, Rain, Snow, Storm };

WxCategory categoryForCode(int code) {
  if (code == 0) return WxCategory::Clear;
  if (code == 1 || code == 2) return WxCategory::PartlyCloudy;
  if (code == 3) return WxCategory::Cloudy;
  if (code == 45 || code == 48) return WxCategory::Fog;
  if (code >= 51 && code <= 57) return WxCategory::Drizzle;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return WxCategory::Rain;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WxCategory::Snow;
  if (code >= 95) return WxCategory::Storm;
  return WxCategory::Cloudy;
}

void drawWeatherIcon(const GfxRenderer& renderer, int code, int x, int y, int size) {
  const WxCategory cat = categoryForCode(code);
  const int r = size / 2;

  if (cat == WxCategory::Clear || cat == WxCategory::PartlyCloudy) {
    // Sun: filled blob + short rays
    renderer.fillRoundedRect(x, y, r, r, r / 2, Color::Black);
    renderer.drawLine(x + r / 2, y - 4, x + r / 2, y - 1);
    renderer.drawLine(x + r / 2, y + r + 1, x + r / 2, y + r + 4);
    renderer.drawLine(x - 4, y + r / 2, x - 1, y + r / 2);
    renderer.drawLine(x + r + 1, y + r / 2, x + r + 4, y + r / 2);
  }
  if (cat == WxCategory::Clear) return;

  // Cloud body for everything else (partly-cloudy layers it under the sun)
  const int cx = cat == WxCategory::PartlyCloudy ? x + r / 2 : x;
  const int cy = cat == WxCategory::PartlyCloudy ? y + r / 2 : y;
  const int cw = size;
  const int ch = size * 2 / 3;
  renderer.fillRoundedRect(cx, cy, cw, ch, ch / 2, Color::Black);

  switch (cat) {
    case WxCategory::Rain:
    case WxCategory::Drizzle:
      for (int i = 0; i < 3; i++) {
        const int dx = cx + 4 + i * (cw - 8) / 2;
        renderer.drawLine(dx, cy + ch + 2, dx - 2, cy + ch + 8, 2, true);
      }
      break;
    case WxCategory::Snow:
      for (int i = 0; i < 3; i++) {
        const int dx = cx + 4 + i * (cw - 8) / 2;
        renderer.fillRect(dx - 1, cy + ch + 3, 3, 3);
      }
      break;
    case WxCategory::Storm:
      renderer.drawLine(cx + cw / 2 + 2, cy + ch, cx + cw / 2 - 3, cy + ch + 5, 2, true);
      renderer.drawLine(cx + cw / 2 - 3, cy + ch + 5, cx + cw / 2 + 1, cy + ch + 5, 2, true);
      renderer.drawLine(cx + cw / 2 + 1, cy + ch + 5, cx + cw / 2 - 4, cy + ch + 10, 2, true);
      break;
    case WxCategory::Fog:
      for (int i = 0; i < 3; i++) {
        renderer.fillRect(cx, cy + ch + 2 + i * 4, cw, 2);
      }
      break;
    default:
      break;
  }
}

void drawWeatherBrandIcon(const GfxRenderer& renderer, int x, int y) { drawWeatherIcon(renderer, 2, x, y, 22); }
}  // namespace

void WeatherDashboardActivity::onEnter() {
  Activity::onEnter();

  if (SETTINGS.weatherZip[0] == '\0') {
    if (autoRefresh) {
      exitDashboardMode();
      finish();
      return;
    }
    promptZip();
    return;
  }

  beginUpdate();
}

void WeatherDashboardActivity::onExit() {
  Activity::onExit();
  if (wifiUsed && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void WeatherDashboardActivity::promptZip() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_ZIP), SETTINGS.weatherZip,
                                              sizeof(SETTINGS.weatherZip) - 1, InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          if (SETTINGS.weatherZip[0] == '\0') {
            finish();
          } else {
            if (state == State::Showing) sleepAt = millis() + DISPLAY_GRACE_INTERACTIVE_MS;
            requestUpdate();
          }
          return;
        }
        const auto& kb = std::get<KeyboardResult>(result.data);
        strncpy(SETTINGS.weatherZip, kb.text.c_str(), sizeof(SETTINGS.weatherZip) - 1);
        SETTINGS.weatherZip[sizeof(SETTINGS.weatherZip) - 1] = '\0';
        SETTINGS.saveToFile();
        if (SETTINGS.weatherZip[0] == '\0') {
          finish();
          return;
        }
        beginUpdate();
      });
}

void WeatherDashboardActivity::beginUpdate() {
  state = State::Connecting;
  errorMessage = nullptr;
  sleepAt = 0;

  if (WiFi.status() == WL_CONNECTED) {
    state = State::Fetching;
    if (!autoRefresh) requestUpdate();
    return;
  }

  wifiUsed = true;
  if (autoRefresh) {
    startDirectWifiConnect();
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             finish();
                             return;
                           }
                           state = State::Fetching;
                           requestUpdate();
                         });
}

void WeatherDashboardActivity::startDirectWifiConnect() {
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* cred = lastSsid.empty() ? nullptr : WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_ERR("WX", "No saved WiFi network for unattended refresh");
    state = State::Failed;
    errorMessage = tr(STR_DASHBOARD_WIFI_FAILED);
    return;
  }

  LOG_INF("WX", "Unattended refresh: connecting to %s", cred->ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());
  wifiConnectStart = millis();
}

void WeatherDashboardActivity::loop() {
  switch (state) {
    case State::Connecting:
      if (!autoRefresh) return;
      if (WiFi.status() == WL_CONNECTED) {
        state = State::Fetching;
        return;
      }
      if (millis() - wifiConnectStart >= WIFI_TIMEOUT_MS) {
        LOG_ERR("WX", "Unattended WiFi connect timed out");
        state = State::Failed;
        errorMessage = tr(STR_DASHBOARD_WIFI_FAILED);
        requestUpdateAndWait();
        goToSleepAndPoll();
      }
      return;

    case State::Fetching:
      if (!autoRefresh) {
        requestUpdateAndWait();
      }
      runFetch();
      return;

    case State::Showing:
    case State::Failed:
      break;
  }

  if (autoRefresh) {
    requestUpdateAndWait();
    goToSleepAndPoll();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitDashboardMode();
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    sleepAt = 0;
    promptZip();
    return;
  }
  if (state == State::Showing && sleepAt != 0 && millis() >= sleepAt) {
    goToSleepAndPoll();
  }
}

bool WeatherDashboardActivity::geocodeIfNeeded() {
  if (strcmp(SETTINGS.weatherZip, SETTINGS.weatherGeocodedZip) == 0 && SETTINGS.weatherLat[0] != '\0') {
    return true;  // cache hit
  }

  char url[80];
  snprintf(url, sizeof(url), "http://api.zippopotam.us/us/%s", SETTINGS.weatherZip);
  std::string body;
  if (!HttpDownloader::fetchUrl(std::string(url), body)) {
    LOG_ERR("WX", "Geocode fetch failed for zip %s", SETTINGS.weatherZip);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
  JsonArrayConst places = doc["places"];
  if (places.isNull() || places.size() == 0) return false;

  const char* lat = places[0]["latitude"] | "";
  const char* lon = places[0]["longitude"] | "";
  const char* place = places[0]["place name"] | "";
  const char* stateAbbrev = places[0]["state abbreviation"] | "";
  if (lat[0] == '\0' || lon[0] == '\0') return false;

  strncpy(SETTINGS.weatherLat, lat, sizeof(SETTINGS.weatherLat) - 1);
  SETTINGS.weatherLat[sizeof(SETTINGS.weatherLat) - 1] = '\0';
  strncpy(SETTINGS.weatherLon, lon, sizeof(SETTINGS.weatherLon) - 1);
  SETTINGS.weatherLon[sizeof(SETTINGS.weatherLon) - 1] = '\0';
  snprintf(SETTINGS.weatherPlaceName, sizeof(SETTINGS.weatherPlaceName), "%s, %s", place, stateAbbrev);
  strncpy(SETTINGS.weatherGeocodedZip, SETTINGS.weatherZip, sizeof(SETTINGS.weatherGeocodedZip) - 1);
  SETTINGS.weatherGeocodedZip[sizeof(SETTINGS.weatherGeocodedZip) - 1] = '\0';
  SETTINGS.saveToFile();
  LOG_INF("WX", "Geocoded %s -> %s,%s (%s)", SETTINGS.weatherZip, lat, lon, SETTINGS.weatherPlaceName);
  return true;
}

bool WeatherDashboardActivity::fetchWeather() {
  char url[300];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,"
           "apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day&daily=weather_code,"
           "temperature_2m_max,temperature_2m_min,precipitation_probability_max&temperature_unit=fahrenheit&wind_"
           "speed_unit=mph&timezone=auto&forecast_days=%d",
           SETTINGS.weatherLat, SETTINGS.weatherLon, MAX_FORECAST_DAYS);

  std::string body;
  if (!HttpDownloader::fetchUrl(std::string(url), body)) {
    LOG_ERR("WX", "Weather fetch failed");
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    LOG_ERR("WX", "Weather JSON parse failed");
    return false;
  }

  JsonObjectConst current = doc["current"];
  if (current.isNull()) return false;
  currentTempF = static_cast<int>(std::lround(current["temperature_2m"] | 0.0));
  feelsLikeF = static_cast<int>(std::lround(current["apparent_temperature"] | 0.0));
  humidityPct = current["relative_humidity_2m"] | 0;
  windMph = static_cast<int>(std::lround(current["wind_speed_10m"] | 0.0));
  currentWeatherCode = current["weather_code"] | 0;
  isDay = (current["is_day"] | 1) != 0;

  forecastCount = 0;
  rainChancePct = 0;
  JsonObjectConst daily = doc["daily"];
  if (!daily.isNull()) {
    JsonArrayConst times = daily["time"];
    JsonArrayConst codes = daily["weather_code"];
    JsonArrayConst highs = daily["temperature_2m_max"];
    JsonArrayConst lows = daily["temperature_2m_min"];
    JsonArrayConst rainProb = daily["precipitation_probability_max"];
    if (rainProb.size() > 0) rainChancePct = rainProb[0] | 0;

    const int n = std::min({(int)times.size(), (int)codes.size(), (int)highs.size(), (int)lows.size(),
                            MAX_FORECAST_DAYS});
    for (int i = 0; i < n; i++) {
      const char* iso = times[i] | "";
      if (iso[0] == '\0') continue;
      const int dow = DashboardUI::dayOfWeekSunday0(iso);
      strncpy(forecast[forecastCount].label, DashboardUI::DAY_ABBREV[dow], 3);
      forecast[forecastCount].label[3] = '\0';
      forecast[forecastCount].hi = static_cast<int>(std::lround(highs[i] | 0.0));
      forecast[forecastCount].lo = static_cast<int>(std::lround(lows[i] | 0.0));
      forecast[forecastCount].weatherCode = codes[i] | 0;
      forecastCount++;
    }
  }
  return true;
}

void WeatherDashboardActivity::runFetch() {
  DashboardUI::syncClockAndTimezone();

  if (!geocodeIfNeeded()) {
    LOG_ERR("WX", "Geocode failed");
    state = State::Failed;
    errorMessage = tr(STR_WEATHER_GEOCODE_FAILED);
  } else if (!fetchWeather()) {
    state = State::Failed;
    errorMessage = tr(STR_WEATHER_FETCH_FAILED);
  } else {
    DashboardUI::formatUpdatedStamp(lastUpdated, sizeof(lastUpdated));
    LOG_INF("WX", "%dF (feels %dF), humidity %d%%, wind %dmph, %d-day forecast", currentTempF, feelsLikeF,
            humidityPct, windMph, forecastCount);
    state = State::Showing;
  }

  requestUpdateAndWait();

  if (autoRefresh) {
    goToSleepAndPoll();
  } else if (state == State::Showing) {
    sleepAt = millis() + DISPLAY_GRACE_INTERACTIVE_MS;
  }
}

void WeatherDashboardActivity::goToSleepAndPoll() {
  APP_STATE.activeDashboardMode = CrossPointState::DASHBOARD_WEATHER;
  APP_STATE.saveToFile();
  const uint32_t intervalS = SETTINGS.weatherRefreshMinutes * 60u;
  LOG_INF("WX", "Dashboard armed, sleeping for %u s", (unsigned)intervalS);
  enterDashboardSleep(intervalS);
}

void WeatherDashboardActivity::exitDashboardMode() {
  if (APP_STATE.activeDashboardMode == CrossPointState::DASHBOARD_WEATHER) {
    APP_STATE.activeDashboardMode = CrossPointState::DASHBOARD_NONE;
    APP_STATE.saveToFile();
  }
}

void WeatherDashboardActivity::render(RenderLock&&) {
  switch (state) {
    case State::Connecting:
    case State::Fetching:
      renderMessage(tr(STR_WEATHER_UPDATING));
      break;
    case State::Failed:
      renderMessage(errorMessage ? errorMessage : tr(STR_WEATHER_FETCH_FAILED));
      break;
    case State::Showing:
      renderDashboard();
      break;
  }
}

void WeatherDashboardActivity::renderMessage(const char* message) const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, tr(STR_WEATHER_DASHBOARD), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 5, message);

  if (!autoRefresh && state == State::Failed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WEATHER_CHANGE_ZIP), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void WeatherDashboardActivity::renderDashboard() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  const auto origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  constexpr int sideMargin = 40;

  renderer.clearScreen();

  // --- Top left: big current temp + place name (TRMNL style) ---
  char hero[8];
  snprintf(hero, sizeof(hero), "%dF", currentTempF);
  renderer.fillRectDither(sideMargin - 16, 42, 8, 74, Color::DarkGray);
  DashboardUI::drawBigText(renderer, sideMargin, 42, hero, 10);
  renderer.drawText(UI_10_FONT_ID, sideMargin, 130, SETTINGS.weatherPlaceName);

  drawWeatherIcon(renderer, currentWeatherCode, sideMargin + 220, 55, 40);

  // --- Top right: 2x2 stat grid ---
  struct StatEntry {
    char value[16];
    const char* label;
  };
  StatEntry stats[4];
  snprintf(stats[0].value, sizeof(stats[0].value), "%dF", feelsLikeF);
  stats[0].label = tr(STR_WEATHER_FEELS_LIKE);
  snprintf(stats[1].value, sizeof(stats[1].value), "%d%%", humidityPct);
  stats[1].label = tr(STR_WEATHER_HUMIDITY);
  snprintf(stats[2].value, sizeof(stats[2].value), "%d", windMph);
  stats[2].label = tr(STR_WEATHER_WIND);
  snprintf(stats[3].value, sizeof(stats[3].value), "%d%%", rainChancePct);
  stats[3].label = tr(STR_WEATHER_RAIN_CHANCE);

  for (int s = 0; s < 4; s++) {
    const int colX = 430 + (s % 2) * 190;
    const int rowY = 42 + (s / 2) * 78;
    renderer.fillRectDither(colX - 16, rowY, 8, 58, Color::DarkGray);
    DashboardUI::drawBigText(renderer, colX, rowY, stats[s].value, 5);
    renderer.drawText(UI_10_FONT_ID, colX, rowY + 40, stats[s].label);
  }

  renderer.fillRect(sideMargin, 196, pageWidth - 2 * sideMargin, 1);

  // --- Forecast row: day + icon + hi/lo, each centered within its own column ---
  if (forecastCount > 0) {
    const int colW = (pageWidth - 2 * sideMargin) / forecastCount;
    for (int i = 0; i < forecastCount; i++) {
      const int cx = sideMargin + i * colW + colW / 2;

      const int labelW = renderer.getTextWidth(UI_10_FONT_ID, forecast[i].label, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, cx - labelW / 2, 224, forecast[i].label, true, EpdFontFamily::BOLD);

      drawWeatherIcon(renderer, forecast[i].weatherCode, cx - 12, 254, 30);

      char line[16];
      snprintf(line, sizeof(line), "%d/%d", forecast[i].hi, forecast[i].lo);
      const int lineW = renderer.getTextWidth(SMALL_FONT_ID, line);
      renderer.drawText(SMALL_FONT_ID, cx - lineW / 2, 302, line);
    }
  }

  // --- Footer bar ---
  DashboardUI::drawFooter(renderer, metrics, pageWidth, pageHeight, sideMargin, drawWeatherBrandIcon,
                          tr(STR_WEATHER_DASHBOARD), tr(STR_DASHBOARD_UPDATED), lastUpdated,
                          SETTINGS.weatherPlaceName);

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  renderer.setOrientation(origOrientation);
}
