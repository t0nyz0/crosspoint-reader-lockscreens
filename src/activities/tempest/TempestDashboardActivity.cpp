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
        LOG_ERR("TMP", "Unattended WiFi connect timed out, leaving last-known dashboard on screen");
        goToSleepAndPoll();  // retry next cycle; nothing valid in memory to redraw with on this fresh boot
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

    if (autoRefresh) {
      // Unattended: a fresh boot's in-memory stats are all zeroed (nothing
      // survives a reboot except what's already physically on the e-ink
      // panel), so there's no valid data to redraw. Skip the render entirely
      // rather than blanking the last-known-good dashboard to an error
      // screen for the whole next sleep interval -- just retry next cycle.
      LOG_INF("TMP", "Leaving last-known dashboard on screen, retrying next cycle");
      goToSleepAndPoll();
      return;
    }
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
  // A non-empty lastUpdated means this exact session already parsed a real
  // observation, so every other stat field is guaranteed valid too (they're
  // set together, right before lastUpdated) -- safe to keep redrawing the
  // full dashboard with a swapped-out footer line instead of blanking to a
  // message screen while a manually-forced refresh is in flight or fails.
  const bool hasCache = lastUpdated[0] != '\0';
  switch (state) {
    case State::Connecting:
    case State::Fetching:
      if (hasCache) {
        renderDashboard(tr(STR_TEMPEST_WAITING), false);
      } else {
        renderMessage(tr(STR_TEMPEST_SEARCHING));
      }
      break;
    case State::Failed:
      if (hasCache) {
        renderDashboard(tr(STR_TEMPEST_REFRESH_FAILED), false);
      } else {
        renderMessage(errorMessage ? errorMessage : tr(STR_TEMPEST_NOT_FOUND));
      }
      break;
    case State::Showing:
      renderDashboard(nullptr, true);
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

void TempestDashboardActivity::renderDashboard(const char* footerStatusOverride, bool isFinal) const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  const bool portrait = SETTINGS.lockScreenOrientation == CrossPointSettings::LOCK_ORIENT_PORTRAIT;
  const auto origOrientation = renderer.getOrientation();
  renderer.setOrientation(portrait ? GfxRenderer::Orientation::Portrait
                                    : GfxRenderer::Orientation::LandscapeCounterClockwise);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int sideMargin = portrait ? 30 : 40;

  renderer.clearScreen();

  const char* label = SETTINGS.tempestLabel[0] != '\0' ? SETTINGS.tempestLabel : tr(STR_TEMPEST_DASHBOARD);
  const DashboardUI::WxCategory condition = localWeatherCategory();
  const char* conditionLabel = localWeatherLabel();

  char hero[8];
  snprintf(hero, sizeof(hero), "%dF", static_cast<int>(tempF + (tempF >= 0 ? 0.5f : -0.5f)));

  // All 12 stat tiles, prepared once and laid out per orientation. Index 0
  // (Wind) also gets the compass dial drawn beside it.
  struct StatEntry {
    char value[16];
    const char* label;
  };
  StatEntry tiles[12];
  snprintf(tiles[0].value, sizeof(tiles[0].value), "%d", static_cast<int>(windAvgMph + 0.5f));
  tiles[0].label = tr(STR_TEMPEST_WIND);
  snprintf(tiles[1].value, sizeof(tiles[1].value), "%d", static_cast<int>(windGustMph + 0.5f));
  tiles[1].label = tr(STR_TEMPEST_GUST);
  snprintf(tiles[2].value, sizeof(tiles[2].value), "%.2f", pressureInHg);
  tiles[2].label = tr(STR_TEMPEST_PRESSURE);
  snprintf(tiles[3].value, sizeof(tiles[3].value), "%d%%", humidityPct);
  tiles[3].label = tr(STR_TEMPEST_HUMIDITY);
  snprintf(tiles[4].value, sizeof(tiles[4].value), "%.2f", rainLastMinIn);
  tiles[4].label = tr(STR_TEMPEST_RAIN);
  snprintf(tiles[5].value, sizeof(tiles[5].value), "%dF",
           static_cast<int>(dewPointF + (dewPointF >= 0 ? 0.5f : -0.5f)));
  tiles[5].label = tr(STR_TEMPEST_DEW_POINT);
  snprintf(tiles[6].value, sizeof(tiles[6].value), "%d", static_cast<int>(uvIndex + 0.5f));
  tiles[6].label = tr(STR_TEMPEST_UV_INDEX);
  if (lightningCount > 0) {
    snprintf(tiles[7].value, sizeof(tiles[7].value), "%d", static_cast<int>(lightningDistMi + 0.5f));
  } else {
    snprintf(tiles[7].value, sizeof(tiles[7].value), "-");
  }
  tiles[7].label = tr(STR_TEMPEST_LIGHTNING);
  DashboardUI::formatCompact(static_cast<uint32_t>(illuminanceLux + 0.5f), tiles[8].value, sizeof(tiles[8].value));
  tiles[8].label = tr(STR_TEMPEST_ILLUMINANCE);
  snprintf(tiles[9].value, sizeof(tiles[9].value), "%d", static_cast<int>(windLullMph + 0.5f));
  tiles[9].label = tr(STR_TEMPEST_WIND_LULL);
  DashboardUI::formatCompact(static_cast<uint32_t>(solarRadiationWm2 + 0.5f), tiles[10].value,
                             sizeof(tiles[10].value));
  tiles[10].label = tr(STR_TEMPEST_SOLAR_RAD);
  if (!pressureTrendValid) {
    snprintf(tiles[11].value, sizeof(tiles[11].value), "-");
    tiles[11].label = tr(STR_TEMPEST_PRESSURE_TREND);
  } else if (pressureTrendDeltaInHg > 0.03f) {
    snprintf(tiles[11].value, sizeof(tiles[11].value), "%.2f", pressureTrendDeltaInHg);
    tiles[11].label = tr(STR_TEMPEST_TREND_RISING);
  } else if (pressureTrendDeltaInHg < -0.03f) {
    snprintf(tiles[11].value, sizeof(tiles[11].value), "-%.2f", -pressureTrendDeltaInHg);
    tiles[11].label = tr(STR_TEMPEST_TREND_FALLING);
  } else {
    snprintf(tiles[11].value, sizeof(tiles[11].value), "%.2f", pressureTrendDeltaInHg);
    tiles[11].label = tr(STR_TEMPEST_TREND_STEADY);
  }

  const char* dir = DashboardUI::compassDirection(windDirDeg);

  // --- Hero + station label (both orientations) ---
  renderer.fillRectDither(sideMargin - 16, 42, 8, 74, Color::DarkGray);
  DashboardUI::drawBigText(renderer, sideMargin, 42, hero, 10);
  renderer.drawText(UI_10_FONT_ID, sideMargin, 130, label);
  // Apparent temp only when it meaningfully differs (else it just echoes temp).
  if (fabsf(feelsLikeF - tempF) >= 2.0f) {
    char feelsLine[24];
    snprintf(feelsLine, sizeof(feelsLine), "%s %dF", tr(STR_TEMPEST_FEELS_LIKE),
             static_cast<int>(feelsLikeF + (feelsLikeF >= 0 ? 0.5f : -0.5f)));
    renderer.drawText(SMALL_FONT_ID, sideMargin, 150, feelsLine);
  }

  if (portrait) {
    // Condition icon top-right of the hero.
    const int condSize = 80;
    const int condX = pageWidth - sideMargin - condSize;
    DashboardUI::drawWeatherIcon(renderer, condition, condX, 46, condSize);
    const int condLabelW = renderer.getTextWidth(UI_10_FONT_ID, conditionLabel);
    renderer.drawText(UI_10_FONT_ID, condX + condSize / 4 - condLabelW / 2, 132, conditionLabel);

    renderer.fillRect(sideMargin, 176, pageWidth - 2 * sideMargin, 1);

    // All 12 tiles in a 2-column stack.
    const int col0 = sideMargin + 16;
    const int col1 = sideMargin + 16 + (pageWidth - 2 * sideMargin) / 2 + 8;
    for (int i = 0; i < 12; i++) {
      const int colX = (i % 2 == 0) ? col0 : col1;
      const int rowY = 198 + (i / 2) * 74;
      DashboardUI::drawStatTile(renderer, colX, rowY, tiles[i].value, tiles[i].label);
      if (i == 0) {
        DashboardUI::drawWindDial(renderer, colX + 92, rowY + 18, 15, windDirDeg);
        const int dirW = renderer.getTextWidth(SMALL_FONT_ID, dir);
        renderer.drawText(SMALL_FONT_ID, colX + 92 - dirW / 2, rowY + 40, dir);
      }
    }
  } else {
    // Condition icon fills the gap between the hero and the top-right grid.
    constexpr int condSize = 110;
    constexpr int condX = 275;
    DashboardUI::drawWeatherIcon(renderer, condition, condX, 42, condSize);
    const int condLabelW = renderer.getTextWidth(UI_10_FONT_ID, conditionLabel);
    renderer.drawText(UI_10_FONT_ID, condX + condSize / 4 - condLabelW / 2, 150, conditionLabel);

    // Primary 2x2 grid, top-right (Wind/Gust, then Pressure/Humidity).
    for (int s = 0; s < 4; s++) {
      const int colX = 430 + (s % 2) * 190;
      const int rowY = 42 + (s / 2) * 78;
      DashboardUI::drawStatTile(renderer, colX, rowY, tiles[s].value, tiles[s].label);
      if (s == 0) {
        DashboardUI::drawWindDial(renderer, colX + 95, rowY + 18, 16, windDirDeg);
        const int dirW = renderer.getTextWidth(SMALL_FONT_ID, dir);
        renderer.drawText(SMALL_FONT_ID, colX + 95 - dirW / 2, rowY + 40, dir);
      }
    }

    renderer.fillRect(sideMargin, 196, pageWidth - 2 * sideMargin, 1);

    // Secondary tiles 4..11 in two rows of four.
    for (int i = 4; i < 12; i++) {
      const int colX = sideMargin + ((i - 4) % 4) * 180;
      const int rowY = 226 + ((i - 4) / 4) * 78;
      DashboardUI::drawStatTile(renderer, colX, rowY, tiles[i].value, tiles[i].label);
    }
  }

  // --- Footer bar: station battery takes the "identity" slot ---
  char battLine[24];
  snprintf(battLine, sizeof(battLine), "%.2fV", stationBatteryV);
  DashboardUI::drawFooter(renderer, metrics, pageWidth, pageHeight, sideMargin, drawTempestBrandIcon, label,
                          footerStatusOverride ? footerStatusOverride : tr(STR_DASHBOARD_UPDATED), lastUpdated,
                          battLine);

  // A fresh reading gets a full refresh (this frame stays up for the whole
  // sleep interval, so it's worth the flash to avoid ghosting). A transient
  // "waiting"/"failed" footer swap over an otherwise-unchanged screen uses a
  // quick partial refresh instead, since it'll be replaced again shortly.
  renderer.displayBuffer(isFinal ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  renderer.setOrientation(origOrientation);
}
