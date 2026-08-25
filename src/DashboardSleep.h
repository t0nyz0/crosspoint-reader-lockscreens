#pragma once

#include <cstdint>

// Timed deep sleep for polling dashboard modes (GitHub/Weather/Tempest, etc;
// defined in main.cpp). Unlike the normal sleep path, this keeps the battery
// latch engaged so the RTC timer can wake the device for the next poll. The
// panel keeps showing the dashboard while asleep. Does not return.
[[noreturn]] void enterDashboardSleep(uint32_t seconds);

// True power-off for a polling dashboard that is too low on battery to keep
// polling. No RTC timer -- wakes only on a power-button press (or USB). The
// caller should render a low-battery frame first; e-ink keeps it visible with
// zero draw. Defined in main.cpp. Does not return.
[[noreturn]] void enterDashboardPowerOff();
