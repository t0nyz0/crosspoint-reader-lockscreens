#pragma once
#include <string>

#include "activities/Activity.h"

// General weather dashboard (Open-Meteo, free/no API key). Shows current
// conditions + a short forecast for a US ZIP code, TRMNL-style, then arms a
// timed deep sleep and re-polls on the configured interval (Settings >
// System > Weather Refresh Interval; default 30 min).
class WeatherDashboardActivity final : public Activity {
 public:
  explicit WeatherDashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoRefresh = false)
      : Activity("WeatherDashboard", renderer, mappedInput), autoRefresh(autoRefresh) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == State::Connecting || state == State::Fetching; }
  bool preventAutoSleep() override { return autoRefresh || state != State::Failed; }

 private:
  enum class State { Connecting, Fetching, Showing, Failed };

  struct ForecastDay {
    char label[4];  // "Mon" etc.
    int hi, lo;
    int weatherCode;
  };

  static constexpr unsigned long WIFI_TIMEOUT_MS = 45000;
  static constexpr unsigned long DISPLAY_GRACE_INTERACTIVE_MS = 20000;
  static constexpr int MAX_FORECAST_DAYS = 5;

  const bool autoRefresh;
  State state = State::Connecting;
  bool wifiUsed = false;
  unsigned long wifiConnectStart = 0;
  unsigned long sleepAt = 0;
  const char* errorMessage = nullptr;

  // Current conditions
  int currentTempF = 0;
  int feelsLikeF = 0;
  int humidityPct = 0;
  int windMph = 0;
  int rainChancePct = 0;
  int currentWeatherCode = 0;
  bool isDay = true;

  ForecastDay forecast[MAX_FORECAST_DAYS];
  int forecastCount = 0;

  char lastUpdated[24] = "";

  void promptZip();
  void beginUpdate();
  void startDirectWifiConnect();
  void runFetch();
  bool geocodeIfNeeded();
  bool fetchWeather();
  void goToSleepAndPoll();
  void exitDashboardMode();

  void renderDashboard() const;
  void renderMessage(const char* message) const;
};
