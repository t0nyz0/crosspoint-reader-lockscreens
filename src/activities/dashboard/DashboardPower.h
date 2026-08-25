#pragma once

#include <cstdint>

// Shared battery/runtime logic for the unattended polling dashboards
// (GitHub/Weather/Tempest). The dashboards wake on a timer, fetch over WiFi,
// render a frame that stays on the panel for the whole sleep interval, then
// sleep again. With no charge awareness they re-arm forever and eventually die
// mid-fetch on a depleting pack ("dashboard dies and you never know why").
//
// This module gives them charge awareness: it reads the battery UN-LOADED
// (before WiFi comes up), persists a small history in RTC memory across deep
// sleep, estimates the discharge rate, and predicts how much runtime is left
// relative to the next refresh -- so the caller can warn, poll less often, or
// pause into a true power-off before the battery can die silently.
namespace DashboardPower {

// NB: PascalCase deliberately -- LOW/HIGH are Arduino macros, so an all-caps
// LOW enumerator would expand to a numeric constant and fail to compile.
enum class Level : uint8_t {
  Ok,        // plenty of charge (or charging)
  Low,       // warn the user and conserve (stretch the interval)
  Critical,  // pause: too low to safely keep polling; power off with a warning frame
};

struct Status {
  uint8_t pct = 0;               // battery percentage (un-loaded reading)
  bool charging = false;         // USB connected
  bool predictionValid = false;  // true once a discharge rate has been learned
  float drainPctPerHour = 0.0f;  // smoothed discharge rate
  float hoursRemaining = 0.0f;   // predicted runtime left (valid iff predictionValid)
  Level level = Level::Ok;
};

// Percent thresholds for the simple (pre-prediction) classification.
constexpr uint8_t CRITICAL_PCT = 7;
constexpr uint8_t LOW_PCT = 20;

// Assess the battery for one unattended poll. Call ONCE per wake, BEFORE
// bringing up WiFi, so the reading is taken un-loaded (WiFi current spikes sag
// the pack voltage and bias the ADC reading low on the X4). Updates the
// RTC-persisted discharge history and fills in a runtime prediction.
//
// `nextIntervalSeconds` is the configured poll interval. If a valid prediction
// says the pack won't survive comfortably until the next poll, the level is
// escalated to CRITICAL so the caller pauses NOW, while there's still charge to
// show a warning, instead of dying mid-fetch next cycle.
Status assess(uint32_t nextIntervalSeconds);

// The poll interval to actually use given the assessed status: unchanged when
// OK, stretched to conserve when LOW.
uint32_t adjustIntervalSeconds(uint32_t baseSeconds, const Status& s);

}  // namespace DashboardPower
