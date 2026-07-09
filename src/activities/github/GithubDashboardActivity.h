#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"

// GitHub contribution dashboard. Shows the profile contribution heatmap
// (like the graph on a GitHub profile page), then arms a timed deep sleep and
// re-polls GitHub once per hour. The e-ink panel keeps showing the dashboard
// while the MCU is asleep; the power button wakes the device and exits the
// mode back to the normal home screen.
class GithubDashboardActivity final : public Activity {
 public:
  explicit GithubDashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoRefresh = false)
      : Activity("GithubDashboard", renderer, mappedInput), autoRefresh(autoRefresh) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == State::Connecting || state == State::Fetching; }
  // Manage sleep ourselves except on the interactive error screen, where the
  // normal inactivity auto-sleep is the safety net.
  bool preventAutoSleep() override { return autoRefresh || state != State::Failed; }

 private:
  enum class State { Connecting, Fetching, Showing, Failed };

  struct ContribCell {
    char date[11];  // "YYYY-MM-DD"
    uint8_t level;  // 0-4 as reported by GitHub
  };

  // Unattended (timer wake) WiFi connect timeout before giving up until next poll
  static constexpr unsigned long WIFI_TIMEOUT_MS = 45000;
  // Interactive window to read the dashboard / back out before the mode arms
  static constexpr unsigned long DISPLAY_GRACE_INTERACTIVE_MS = 20000;

  const bool autoRefresh;
  State state = State::Connecting;
  bool wifiUsed = false;
  unsigned long wifiConnectStart = 0;
  unsigned long sleepAt = 0;  // 0 = sleep not armed
  const char* errorMessage = nullptr;

  // 54 week columns x 7 rows covers any contribution calendar
  static constexpr size_t MAX_SLOTS = 384;

  std::vector<ContribCell> cells;
  std::string totalContributions;  // e.g. "3,640" from the heading (fallback)
  std::string parseBuf;            // streaming HTML carry-over buffer
  uint16_t counts[MAX_SLOTS] = {};  // per-day counts from tool-tips, keyed by table slot (col*7+row)
  bool countsFound = false;

  // Stats computed after a successful fetch
  uint32_t statTotal = 0;
  uint16_t statMostInDay = 0;
  int statLongestStreak = 0;
  int statCurrentStreak = 0;

  // "Updated Jul 8 14:32" stamp captured after each successful fetch, from the
  // ESP32's internal clock (SNTP-synced; the X4 has no hardware RTC).
  char lastUpdated[24] = "";

  void promptUsername();
  void beginUpdate();
  void startDirectWifiConnect();
  void runFetch();
  bool feedHtml(const uint8_t* data, size_t len);
  void scanForTotal();
  void goToSleepAndPoll();
  void exitDashboardMode();

  void computeStats();
  bool cellContributed(size_t i, int offset) const;
  void renderDashboard() const;
  void renderMessage(const char* message) const;
};
