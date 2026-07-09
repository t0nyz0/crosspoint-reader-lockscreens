#pragma once
#include <string>

#include "activities/Activity.h"
#include "activities/dashboard/DashboardUI.h"

// Local WeatherFlow Tempest station dashboard. Listens for the station's
// broadcast UDP packets on the home WiFi network (no cloud API, no token,
// no setup) and shows live conditions TRMNL-style, then arms a timed deep
// sleep and re-polls on the configured interval (Settings > System > Tempest
// Refresh Interval; default 10 min). Requires the reader and the Tempest hub
// to be on the same local network.
class TempestDashboardActivity final : public Activity {
 public:
  explicit TempestDashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoRefresh = false)
      : Activity("TempestDashboard", renderer, mappedInput), autoRefresh(autoRefresh) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == State::Connecting || state == State::Fetching; }
  bool preventAutoSleep() override { return autoRefresh || state != State::Failed; }

 private:
  enum class State { Connecting, Fetching, Showing, Failed };

  static constexpr unsigned long WIFI_TIMEOUT_MS = 45000;
  static constexpr unsigned long DISPLAY_GRACE_INTERACTIVE_MS = 20000;
  // The hub's default obs_st report interval is 60s; listen long enough to
  // reliably catch one broadcast even if we just missed the last one.
  static constexpr unsigned long UDP_LISTEN_TIMEOUT_MS = 65000;
  static constexpr uint16_t TEMPEST_UDP_PORT = 50222;

  const bool autoRefresh;
  State state = State::Connecting;
  bool wifiUsed = false;
  unsigned long wifiConnectStart = 0;
  unsigned long sleepAt = 0;
  const char* errorMessage = nullptr;

  // Latest observation (obs_st), converted to US units
  float tempF = 0;
  int humidityPct = 0;
  float windAvgMph = 0;
  float windGustMph = 0;
  int windDirDeg = 0;
  float pressureInHg = 0;
  float rainLastMinIn = 0;
  float stationBatteryV = 0;
  // Not shown directly; feed the local Clear/Cloudy/etc. heuristic (Tempest's
  // local broadcast has no interpreted condition, unlike a cloud weather API).
  float uvIndex = 0;
  float solarRadiationWm2 = 0;
  int precipType = 0;  // 0 = none, 1 = rain, 2 = hail, 3 = rain+hail

  char lastUpdated[24] = "";

  void promptLabel();
  void beginUpdate();
  void startDirectWifiConnect();
  void runFetch();
  bool listenForObservation();
  void goToSleepAndPoll();

  void renderDashboard() const;
  void renderMessage(const char* message) const;

  DashboardUI::WxCategory localWeatherCategory() const;
  const char* localWeatherLabel() const;
};
