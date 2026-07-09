#pragma once
#include <cstdint>
#include <mutex>
#include <string>

class CrossPointState {
  mutable std::mutex _mutex;

  // Static instance
  static CrossPointState instance;

 public:
  // Access the state mutex for protecting multi-field reads/writes from other cores.
  std::mutex& getMutex() const { return _mutex; }

  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;

  // Which timed-poll dashboard (if any) is armed for its hourly/periodic timed
  // deep sleep cycle. Boot uses this to route an RTC timer wake back into the
  // right dashboard activity; any other wake reason clears it back to NONE.
  enum ActiveDashboard : uint8_t { DASHBOARD_NONE = 0, DASHBOARD_GITHUB = 1, DASHBOARD_WEATHER = 2, DASHBOARD_TEMPEST = 3 };
  uint8_t activeDashboardMode = DASHBOARD_NONE;

  // Reference point for the Tempest dashboard's ~3-hour pressure tendency
  // (classic "rising/falling/steady" reading). Refreshed only once the
  // reference gets old, so short polling intervals still measure a
  // meaningful window instead of a noisy few-minute delta.
  float tempestTrendRefPressureInHg = 0;
  uint32_t tempestTrendRefEpoch = 0;

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
