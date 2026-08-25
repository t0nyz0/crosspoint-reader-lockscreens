#include "DashboardPower.h"

#include <Arduino.h>  // RTC_DATA_ATTR, delay()
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <ctime>

// RTC slow-memory discharge history. RTC_DATA_ATTR survives deep sleep (the
// timed dashboard sleep keeps the MCU powered so the RTC timer can fire) and is
// zero-initialized on a true power-on / battery pull -- so a fresh boot starts
// with no misleading history and re-learns the rate.
RTC_DATA_ATTR static uint32_t s_lastEpoch;        // wall-clock secs at the last SoC change
RTC_DATA_ATTR static uint16_t s_lastPct;          // battery % at that point
RTC_DATA_ATTR static float s_drainPctPerHour;     // smoothed discharge rate
RTC_DATA_ATTR static bool s_haveHistory;          // a usable baseline exists
RTC_DATA_ATTR static uint8_t s_rateSamples;       // # of measured drops folded in; >=2 => trustworthy

namespace DashboardPower {

namespace {
// Pause if the predicted runtime won't cover this many times the next interval
// -- leaves margin to render the warning and avoid a brownout mid-fetch.
constexpr float SURVIVAL_MARGIN = 1.5f;
constexpr float MIN_DELTA_HOURS = 0.02f;  // ignore <~1min gaps (noise / rapid repaint)
constexpr float MAX_PLAUSIBLE_RATE = 50.0f;  // %/hour; anything higher is a glitch
constexpr float RATE_ALPHA = 0.4f;           // EWMA weight for a new rate sample
constexpr uint8_t RISE_NOISE_PCT = 3;        // a % rise smaller than this is ADC noise
constexpr uint32_t LOW_MIN_INTERVAL_S = 2 * 3600;  // when LOW, poll at most every 2h

uint8_t readUnloadedPercent() {
  // On the X4 ADC path getBatteryPercentage() runs an EWMA, so prime it with a
  // few reads; on X3 the fuel-gauge value is already stable. Caller invokes this
  // before WiFi, so the pack isn't sagging under radio load.
  uint16_t pct = powerManager.getBatteryPercentage();
  for (int i = 0; i < 3; i++) {
    delay(15);
    pct = powerManager.getBatteryPercentage();
  }
  return pct > 100 ? 100 : static_cast<uint8_t>(pct);
}
}  // namespace

Status assess(uint32_t nextIntervalSeconds) {
  Status s;
  s.pct = readUnloadedPercent();
  s.charging = gpio.isUsbConnected();

  const time_t now = time(nullptr);
  const bool clockValid = now > 1735689600;  // >= 2025-01-01, i.e. clock has been set

  if (s.charging) {
    // Charging invalidates the discharge model; drop the baseline so we re-learn
    // cleanly after the next unplug.
    s_drainPctPerHour = 0.0f;
    s_haveHistory = false;
    s_rateSamples = 0;
  } else if (clockValid) {
    const uint32_t nowS = static_cast<uint32_t>(now);
    if (!s_haveHistory || s_lastEpoch == 0 || nowS <= s_lastEpoch ||
        (s.pct > s_lastPct && (s.pct - s_lastPct) > RISE_NOISE_PCT)) {
      // No usable prior point, clock moved backwards, or a real rise (partial
      // charge) -- (re)establish the baseline here.
      s_lastEpoch = nowS;
      s_lastPct = s.pct;
      s_haveHistory = true;
    } else if (s.pct < s_lastPct) {
      // A real drop: measure the rate over the actual elapsed time since the
      // last change, fold it into the smoothed estimate, and move the baseline.
      const float hours = static_cast<float>(nowS - s_lastEpoch) / 3600.0f;
      if (hours >= MIN_DELTA_HOURS) {
        const float rate = static_cast<float>(s_lastPct - s.pct) / hours;
        if (rate > 0.0f && rate < MAX_PLAUSIBLE_RATE) {
          s_drainPctPerHour = s_drainPctPerHour > 0.0f
                                  ? (s_drainPctPerHour * (1.0f - RATE_ALPHA) + rate * RATE_ALPHA)
                                  : rate;
          if (s_rateSamples < 255) s_rateSamples++;
        }
        s_lastEpoch = nowS;
        s_lastPct = s.pct;
      }
    }
    // s.pct == s_lastPct: keep the baseline and let time accumulate.
  }

  // A prediction is only trusted once at least two real drops have been measured
  // (>=2 samples) -- a single noisy reading must never trigger a power-off.
  if (!s.charging && s_drainPctPerHour > 0.0f && s_rateSamples >= 2) {
    s.predictionValid = true;
    s.drainPctPerHour = s_drainPctPerHour;
    s.hoursRemaining = static_cast<float>(s.pct) / s_drainPctPerHour;
  }

  // Classify. CRITICAL_PCT is the hard floor. Above it, pause only if a trusted
  // prediction says the pack won't survive the interval we'd ACTUALLY sleep --
  // which, when Low, is the stretched interval (see adjustIntervalSeconds), not
  // the base -- otherwise we could stretch into a sleep the battery can't finish.
  const uint32_t sleepS = nextIntervalSeconds > LOW_MIN_INTERVAL_S ? nextIntervalSeconds : LOW_MIN_INTERVAL_S;
  const bool wontSurvive =
      s.predictionValid && s.hoursRemaining * 3600.0f < static_cast<float>(sleepS) * SURVIVAL_MARGIN;
  if (s.charging) {
    s.level = Level::Ok;
  } else if (s.pct <= CRITICAL_PCT || wontSurvive) {
    s.level = Level::Critical;
  } else if (s.pct <= LOW_PCT) {
    s.level = Level::Low;
  } else {
    s.level = Level::Ok;
  }
  LOG_INF("BATT", "assess pct=%u chg=%d pred=%d drain=%.1f/h left=%.1fh lvl=%d intv=%us", (unsigned)s.pct,
          (int)s.charging, (int)s.predictionValid, s.drainPctPerHour, s.hoursRemaining, (int)s.level,
          (unsigned)nextIntervalSeconds);
  return s;
}

uint32_t adjustIntervalSeconds(uint32_t baseSeconds, const Status& s) {
  if (s.level == Level::Low && baseSeconds < LOW_MIN_INTERVAL_S) {
    return LOW_MIN_INTERVAL_S;
  }
  return baseSeconds;
}

}  // namespace DashboardPower
