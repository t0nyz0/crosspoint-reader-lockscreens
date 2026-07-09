#include "TempestDashboardActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

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

namespace {
// Small weather-vane glyph for the footer branding: a diamond with a stem.
void drawTempestBrandIcon(const GfxRenderer& renderer, int x, int y) {
  renderer.drawLine(x + 11, y, x + 3, y + 8, 2, true);
  renderer.drawLine(x + 11, y, x + 19, y + 8, 2, true);
  renderer.drawLine(x + 11, y + 22, x + 3, y + 14, 2, true);
  renderer.drawLine(x + 11, y + 22, x + 19, y + 14, 2, true);
  renderer.fillRoundedRect(x + 9, y + 9, 5, 5, 2, Color::Black);
}
}  // namespace

// Tempest's local UDP broadcast has no interpreted sky condition (that's only
// in WeatherFlow's cloud API, which we deliberately avoid needing). This is a
// best-effort local approximation from precip type + UV + solar radiation --
// it can't distinguish clear-vs-cloudy skies at night, so it defaults to
// Clear after dark rather than guessing further.
DashboardUI::WxCategory TempestDashboardActivity::localWeatherCategory() const {
  if (precipType == 2) return DashboardUI::WxCategory::Snow;  // hail
  if (precipType != 0) return DashboardUI::WxCategory::Rain;  // rain or rain+hail
  if (solarRadiationWm2 < 3 && uvIndex < 0.3f) return DashboardUI::WxCategory::Clear;  // likely night
  if (uvIndex >= 3.0f && solarRadiationWm2 >= 400) return DashboardUI::WxCategory::Clear;
  if (solarRadiationWm2 >= 120) return DashboardUI::WxCategory::PartlyCloudy;
  return DashboardUI::WxCategory::Cloudy;
}

const char* TempestDashboardActivity::localWeatherLabel() const {
  switch (localWeatherCategory()) {
    case DashboardUI::WxCategory::PartlyCloudy:
      return tr(STR_TEMPEST_WX_PARTLY_CLOUDY);
    case DashboardUI::WxCategory::Cloudy:
      return tr(STR_TEMPEST_WX_CLOUDY);
    case DashboardUI::WxCategory::Rain:
      return tr(STR_TEMPEST_WX_RAIN);
    case DashboardUI::WxCategory::Snow:
      return tr(STR_TEMPEST_WX_SNOW);
    default:
      return tr(STR_TEMPEST_WX_CLEAR);
  }
}

void TempestDashboardActivity::onEnter() {
  Activity::onEnter();
  beginUpdate();
}

void TempestDashboardActivity::onExit() {
  Activity::onExit();
  if (wifiUsed && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void TempestDashboardActivity::promptLabel() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TEMPEST_LABEL), SETTINGS.tempestLabel,
                                              sizeof(SETTINGS.tempestLabel) - 1, InputType::Text),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& kb = std::get<KeyboardResult>(result.data);
          strncpy(SETTINGS.tempestLabel, kb.text.c_str(), sizeof(SETTINGS.tempestLabel) - 1);
          SETTINGS.tempestLabel[sizeof(SETTINGS.tempestLabel) - 1] = '\0';
          SETTINGS.saveToFile();
        }
        if (state == State::Showing) sleepAt = millis() + DISPLAY_GRACE_INTERACTIVE_MS;
        requestUpdate();
      });
}

void TempestDashboardActivity::beginUpdate() {
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

void TempestDashboardActivity::startDirectWifiConnect() {
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* cred = lastSsid.empty() ? nullptr : WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_ERR("TMP", "No saved WiFi network for unattended refresh");
    state = State::Failed;
    errorMessage = tr(STR_DASHBOARD_WIFI_FAILED);
    return;
  }

  LOG_INF("TMP", "Unattended refresh: connecting to %s", cred->ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());
  wifiConnectStart = millis();
}

void TempestDashboardActivity::loop() {
  switch (state) {
    case State::Connecting:
      if (!autoRefresh) return;
      if (WiFi.status() == WL_CONNECTED) {
        state = State::Fetching;
        return;
      }
      if (millis() - wifiConnectStart >= WIFI_TIMEOUT_MS) {
        LOG_ERR("TMP", "Unattended WiFi connect timed out");
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
    if (APP_STATE.activeDashboardMode == CrossPointState::DASHBOARD_TEMPEST) {
      APP_STATE.activeDashboardMode = CrossPointState::DASHBOARD_NONE;
      APP_STATE.saveToFile();
    }
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    sleepAt = 0;
    promptLabel();
    return;
  }
  if (state == State::Showing && sleepAt != 0 && millis() >= sleepAt) {
    goToSleepAndPoll();
  }
}

bool TempestDashboardActivity::listenForObservation() {
  WiFiUDP udp;
  if (!udp.begin(TEMPEST_UDP_PORT)) {
    LOG_ERR("TMP", "Failed to bind UDP port %u", TEMPEST_UDP_PORT);
    return false;
  }

  const unsigned long start = millis();
  static char packetBuf[768];
  bool found = false;

  while (millis() - start < UDP_LISTEN_TIMEOUT_MS) {
    const int packetSize = udp.parsePacket();
    if (packetSize <= 0) {
      delay(50);
      continue;
    }

    const int len = udp.read(packetBuf, sizeof(packetBuf) - 1);
    if (len <= 0) continue;
    packetBuf[len] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, packetBuf) != DeserializationError::Ok) continue;
    if (strcmp(doc["type"] | "", "obs_st") != 0) continue;

    JsonArrayConst obs = doc["obs"][0];
    if (obs.isNull() || obs.size() < 17) continue;

    // Read every field via a float default: ArduinoJson's `| default` picks
    // the fallback whenever the stored JSON type doesn't match the default's
    // type, and the wire format encodes wind direction/humidity as floats
    // (e.g. 58.26) even though they're conceptually whole numbers -- an int
    // default there silently returns 0 instead of coercing. Round afterward
    // for the fields that are logically integers.
    const float windLullMs = obs[1] | 0.0f;
    const float windAvgMs = obs[2] | 0.0f;
    const float windGustMs = obs[3] | 0.0f;
    const float windDirRaw = obs[4] | 0.0f;
    const float pressureMb = obs[6] | 0.0f;
    const float tempC = obs[7] | 0.0f;
    const float humidityRaw = obs[8] | 0.0f;
    const float illuminanceRaw = obs[9] | 0.0f;
    const float uvRaw = obs[10] | 0.0f;
    const float solarRaw = obs[11] | 0.0f;
    const float rainMm = obs[12] | 0.0f;
    const float precipTypeRaw = obs[13] | 0.0f;
    const float lightningDistKm = obs[14] | 0.0f;
    const float lightningCountRaw = obs[15] | 0.0f;
    stationBatteryV = obs[16] | 0.0f;

    windDirDeg = static_cast<int>(windDirRaw + 0.5f);
    humidityPct = static_cast<int>(humidityRaw + 0.5f);
    illuminanceLux = illuminanceRaw;
    uvIndex = uvRaw;
    solarRadiationWm2 = solarRaw;
    precipType = static_cast<int>(precipTypeRaw + 0.5f);
    lightningCount = static_cast<int>(lightningCountRaw + 0.5f);
    lightningDistMi = lightningDistKm * 0.621371f;
    windLullMph = windLullMs * 2.23694f;
    windAvgMph = windAvgMs * 2.23694f;
    windGustMph = windGustMs * 2.23694f;
    pressureInHg = pressureMb * 0.0295300f;
    rainLastMinIn = rainMm * 0.0393701f;
    tempF = tempC * 9.0f / 5.0f + 32.0f;

    // Dew point via the Magnus-Tetens approximation, computed in Celsius
    // (matches the wire units) then converted to F for display.
    const float rh = humidityRaw / 100.0f;
    if (rh > 0.0f) {
      const float alpha = (17.27f * tempC) / (237.7f + tempC) + logf(rh);
      const float dewC = (237.7f * alpha) / (17.27f - alpha);
      dewPointF = dewC * 9.0f / 5.0f + 32.0f;
    } else {
      dewPointF = tempF;
    }

    // Apparent temperature: NWS wind chill below 50F with wind, heat index
    // above 80F, otherwise the same as the actual reading.
    if (tempF <= 50.0f && windAvgMph >= 3.0f) {
      const float v016 = powf(windAvgMph, 0.16f);
      feelsLikeF = 35.74f + 0.6215f * tempF - 35.75f * v016 + 0.4275f * tempF * v016;
    } else if (tempF >= 80.0f) {
      const float T = tempF;
      const float RH = humidityRaw;
      float hi = 0.5f * (T + 61.0f + (T - 68.0f) * 1.2f + RH * 0.094f);
      if ((hi + T) / 2.0f >= 80.0f) {
        hi = -42.379f + 2.04901523f * T + 10.14333127f * RH - 0.22475541f * T * RH - 0.00683783f * T * T -
             0.05481717f * RH * RH + 0.00122874f * T * T * RH + 0.00085282f * T * RH * RH -
             0.00000199f * T * T * RH * RH;
        if (RH < 13.0f && T <= 112.0f) {
          hi -= ((13.0f - RH) / 4.0f) * sqrtf((17.0f - fabsf(T - 95.0f)) / 17.0f);
        } else if (RH > 85.0f && T <= 87.0f) {
          hi += ((RH - 85.0f) / 10.0f) * ((87.0f - T) / 5.0f);
        }
      }
      feelsLikeF = hi;
    } else {
      feelsLikeF = tempF;
    }

    found = true;
    break;
  }

  udp.stop();
  return found;
}

// Classic "rising/falling/steady" pressure tendency, tracked against a
// reference reading persisted across sleep cycles in CrossPointState. The
// reference only refreshes every ~3 hours (the standard meteorological
// tendency window), so a short poll interval still measures a meaningful
// span instead of a noisy few-minute delta.
void TempestDashboardActivity::computePressureTrend() {
  constexpr uint32_t TREND_MIN_WINDOW_S = 30 * 60;
  constexpr uint32_t TREND_RESET_WINDOW_S = 3 * 60 * 60;

  pressureTrendValid = false;
  pressureTrendDeltaInHg = 0;

  const time_t now = time(nullptr);
  if (now <= 1735689600) return;  // clock not synced; don't build a trend off a bogus timestamp

  if (APP_STATE.tempestTrendRefEpoch == 0 || now <= static_cast<time_t>(APP_STATE.tempestTrendRefEpoch)) {
    APP_STATE.tempestTrendRefPressureInHg = pressureInHg;
    APP_STATE.tempestTrendRefEpoch = static_cast<uint32_t>(now);
    APP_STATE.saveToFile();
    return;
  }

  const uint32_t age = static_cast<uint32_t>(now - static_cast<time_t>(APP_STATE.tempestTrendRefEpoch));
  if (age >= TREND_MIN_WINDOW_S) {
    pressureTrendValid = true;
    pressureTrendDeltaInHg = pressureInHg - APP_STATE.tempestTrendRefPressureInHg;
  }
  if (age >= TREND_RESET_WINDOW_S) {
    APP_STATE.tempestTrendRefPressureInHg = pressureInHg;
    APP_STATE.tempestTrendRefEpoch = static_cast<uint32_t>(now);
    APP_STATE.saveToFile();
  }
}

void TempestDashboardActivity::runFetch() {
  DashboardUI::syncClockAndTimezone();

  LOG_INF("TMP", "Listening for Tempest broadcast on UDP %u (up to %lu ms)", TEMPEST_UDP_PORT,
          UDP_LISTEN_TIMEOUT_MS);

  if (!listenForObservation()) {
    LOG_ERR("TMP", "No Tempest observation received");
    state = State::Failed;
    errorMessage = tr(STR_TEMPEST_NOT_FOUND);
  } else {
    computePressureTrend();
    DashboardUI::formatUpdatedStamp(lastUpdated, sizeof(lastUpdated));
    LOG_INF("TMP", "%.1fF, humidity %d%%, wind %.1f mph %s, pressure %.2f inHg", tempF, humidityPct, windAvgMph,
            DashboardUI::compassDirection(windDirDeg), pressureInHg);
    state = State::Showing;
  }

  requestUpdateAndWait();

  if (autoRefresh) {
    goToSleepAndPoll();
  } else if (state == State::Showing) {
    sleepAt = millis() + DISPLAY_GRACE_INTERACTIVE_MS;
  }
}

void TempestDashboardActivity::goToSleepAndPoll() {
  APP_STATE.activeDashboardMode = CrossPointState::DASHBOARD_TEMPEST;
  APP_STATE.saveToFile();
  const uint32_t intervalS = SETTINGS.tempestRefreshMinutes * 60u;
  LOG_INF("TMP", "Dashboard armed, sleeping for %u s", (unsigned)intervalS);
  enterDashboardSleep(intervalS);
}

void TempestDashboardActivity::render(RenderLock&&) {
  switch (state) {
    case State::Connecting:
      renderMessage(tr(STR_TEMPEST_SEARCHING));
      break;
    case State::Fetching:
      renderMessage(tr(STR_TEMPEST_SEARCHING));
      break;
    case State::Failed:
      renderMessage(errorMessage ? errorMessage : tr(STR_TEMPEST_NOT_FOUND));
      break;
    case State::Showing:
      renderDashboard();
      break;
  }
}

void TempestDashboardActivity::renderMessage(const char* message) const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  const char* label = SETTINGS.tempestLabel[0] != '\0' ? SETTINGS.tempestLabel : tr(STR_TEMPEST_DASHBOARD);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, label, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 5, message);

  if (state == State::Failed) {
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 30, tr(STR_TEMPEST_NOT_FOUND_HINT));
  }

  if (!autoRefresh && state == State::Failed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void TempestDashboardActivity::renderDashboard() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  const auto origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  constexpr int sideMargin = 40;

  renderer.clearScreen();

  const char* label = SETTINGS.tempestLabel[0] != '\0' ? SETTINGS.tempestLabel : tr(STR_TEMPEST_DASHBOARD);

  // --- Top left: big current temp + station label ---
  char hero[8];
  snprintf(hero, sizeof(hero), "%dF", static_cast<int>(tempF + (tempF >= 0 ? 0.5f : -0.5f)));
  renderer.fillRectDither(sideMargin - 16, 42, 8, 74, Color::DarkGray);
  DashboardUI::drawBigText(renderer, sideMargin, 42, hero, 10);
  renderer.drawText(UI_10_FONT_ID, sideMargin, 130, label);

  // Apparent temperature, only when it meaningfully differs from the actual
  // reading (heat index / wind chill both collapse to the actual temp
  // otherwise, so there's nothing worth stating).
  if (fabsf(feelsLikeF - tempF) >= 2.0f) {
    char feelsLine[24];
    snprintf(feelsLine, sizeof(feelsLine), "%s %dF", tr(STR_TEMPEST_FEELS_LIKE),
             static_cast<int>(feelsLikeF + (feelsLikeF >= 0 ? 0.5f : -0.5f)));
    renderer.drawText(SMALL_FONT_ID, sideMargin, 150, feelsLine);
  }

  // --- Condition icon + label, between the hero and the stat grid ---
  // Tempest's local broadcast has no interpreted sky condition (unlike a
  // cloud weather API), so this is a best-effort local approximation --
  // see localWeatherCategory().
  const DashboardUI::WxCategory condition = localWeatherCategory();
  const char* conditionLabel = localWeatherLabel();
  constexpr int conditionIconX = 260;
  DashboardUI::drawWeatherIcon(renderer, condition, conditionIconX, 40, 44);
  const int conditionLabelW = renderer.getTextWidth(UI_10_FONT_ID, conditionLabel);
  renderer.drawText(UI_10_FONT_ID, conditionIconX + 22 - conditionLabelW / 2, 100, conditionLabel);

  // --- Top right: 2x2 stat grid ---
  struct StatEntry {
    char value[16];
    const char* label;
  };
  StatEntry stats[4];
  snprintf(stats[0].value, sizeof(stats[0].value), "%d", static_cast<int>(windAvgMph + 0.5f));
  stats[0].label = tr(STR_TEMPEST_WIND);
  snprintf(stats[1].value, sizeof(stats[1].value), "%d%%", humidityPct);
  stats[1].label = tr(STR_TEMPEST_HUMIDITY);
  snprintf(stats[2].value, sizeof(stats[2].value), "%.2f", pressureInHg);
  stats[2].label = tr(STR_TEMPEST_PRESSURE);
  snprintf(stats[3].value, sizeof(stats[3].value), "%.2f", rainLastMinIn);
  stats[3].label = tr(STR_TEMPEST_RAIN);

  for (int s = 0; s < 4; s++) {
    const int colX = 430 + (s % 2) * 190;
    const int rowY = 42 + (s / 2) * 78;
    renderer.fillRectDither(colX - 16, rowY, 8, 58, Color::DarkGray);
    DashboardUI::drawBigText(renderer, colX, rowY, stats[s].value, 5);
    renderer.drawText(UI_10_FONT_ID, colX, rowY + 40, stats[s].label);
    if (s == 0) {
      // Wind direction dial beside the speed digits, clear of the next column.
      DashboardUI::drawWindDial(renderer, colX + 95, rowY + 18, 16, windDirDeg);
      const char* dir = DashboardUI::compassDirection(windDirDeg);
      const int dirW = renderer.getTextWidth(SMALL_FONT_ID, dir);
      renderer.drawText(SMALL_FONT_ID, colX + 95 - dirW / 2, rowY + 40, dir);
    }
  }

  renderer.fillRect(sideMargin, 196, pageWidth - 2 * sideMargin, 1);

  // --- Second stat row: more live local readings (dew point, UV, gust,
  // lightning). Tempest's local broadcast has no forecast -- that only
  // exists in WeatherFlow's cloud API -- so this fills the space with more
  // of what the station itself actually reports, same tile style as above.
  StatEntry row2[4];
  snprintf(row2[0].value, sizeof(row2[0].value), "%dF", static_cast<int>(dewPointF + (dewPointF >= 0 ? 0.5f : -0.5f)));
  row2[0].label = tr(STR_TEMPEST_DEW_POINT);
  snprintf(row2[1].value, sizeof(row2[1].value), "%d", static_cast<int>(uvIndex + 0.5f));
  row2[1].label = tr(STR_TEMPEST_UV_INDEX);
  snprintf(row2[2].value, sizeof(row2[2].value), "%d", static_cast<int>(windGustMph + 0.5f));
  row2[2].label = tr(STR_TEMPEST_GUST);
  if (lightningCount > 0) {
    snprintf(row2[3].value, sizeof(row2[3].value), "%d", static_cast<int>(lightningDistMi + 0.5f));
  } else {
    snprintf(row2[3].value, sizeof(row2[3].value), "-");
  }
  row2[3].label = tr(STR_TEMPEST_LIGHTNING);

  for (int s = 0; s < 4; s++) {
    const int colX = sideMargin + s * 180;
    constexpr int rowY = 226;
    renderer.fillRectDither(colX - 16, rowY, 8, 58, Color::DarkGray);
    DashboardUI::drawBigText(renderer, colX, rowY, row2[s].value, 5);
    renderer.drawText(UI_10_FONT_ID, colX, rowY + 40, row2[s].label);
  }

  // --- Third stat row: illuminance, wind lull, solar radiation, and the
  // ~3-hour pressure tendency -- again all local, zero extra network calls.
  StatEntry row3[4];
  DashboardUI::formatCompact(static_cast<uint32_t>(illuminanceLux + 0.5f), row3[0].value, sizeof(row3[0].value));
  row3[0].label = tr(STR_TEMPEST_ILLUMINANCE);
  snprintf(row3[1].value, sizeof(row3[1].value), "%d", static_cast<int>(windLullMph + 0.5f));
  row3[1].label = tr(STR_TEMPEST_WIND_LULL);
  DashboardUI::formatCompact(static_cast<uint32_t>(solarRadiationWm2 + 0.5f), row3[2].value, sizeof(row3[2].value));
  row3[2].label = tr(STR_TEMPEST_SOLAR_RAD);
  const char* trendLabelText;
  if (!pressureTrendValid) {
    snprintf(row3[3].value, sizeof(row3[3].value), "-");
    trendLabelText = tr(STR_TEMPEST_PRESSURE_TREND);
  } else if (pressureTrendDeltaInHg > 0.03f) {
    snprintf(row3[3].value, sizeof(row3[3].value), "%.2f", pressureTrendDeltaInHg);
    trendLabelText = tr(STR_TEMPEST_TREND_RISING);
  } else if (pressureTrendDeltaInHg < -0.03f) {
    snprintf(row3[3].value, sizeof(row3[3].value), "-%.2f", -pressureTrendDeltaInHg);
    trendLabelText = tr(STR_TEMPEST_TREND_FALLING);
  } else {
    snprintf(row3[3].value, sizeof(row3[3].value), "%.2f", pressureTrendDeltaInHg);
    trendLabelText = tr(STR_TEMPEST_TREND_STEADY);
  }
  row3[3].label = trendLabelText;

  for (int s = 0; s < 4; s++) {
    const int colX = sideMargin + s * 180;
    constexpr int rowY = 304;
    renderer.fillRectDither(colX - 16, rowY, 8, 58, Color::DarkGray);
    DashboardUI::drawBigText(renderer, colX, rowY, row3[s].value, 5);
    renderer.drawText(UI_10_FONT_ID, colX, rowY + 40, row3[s].label);
  }

  // --- Footer bar: station battery takes the "identity" slot ---
  char battLine[24];
  snprintf(battLine, sizeof(battLine), "%.2fV", stationBatteryV);
  DashboardUI::drawFooter(renderer, metrics, pageWidth, pageHeight, sideMargin, drawTempestBrandIcon, label,
                          tr(STR_DASHBOARD_UPDATED), lastUpdated, battLine);

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  renderer.setOrientation(origOrientation);
}
