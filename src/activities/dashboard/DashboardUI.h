#pragma once
#include <cstddef>
#include <cstdint>

class GfxRenderer;
struct ThemeMetrics;

// Shared TRMNL-style rendering helpers for the polling dashboards
// (GitHub / Weather / Tempest): a chunky dot-matrix "hero number" font, a
// compact-count formatter, and the common footer bar (brand icon + label +
// last-updated stamp + identity string + battery).
namespace DashboardUI {

// Draw text using 5x7 dot-matrix glyphs (0-9, '.', '-', 'k', 'F', '%').
// Unsupported characters advance the cursor but draw nothing.
void drawBigText(const GfxRenderer& renderer, int x, int y, const char* text, int dot);
int bigTextWidth(const char* text, int dot);

// "3640" -> "3.6k" for values >= 1000 (hero-number display); small values pass through.
void formatCompact(uint32_t n, char* out, size_t outLen);

// Function pointer to draw a small (~26px) footer brand mark at (x, y), top-left origin.
using BrandIconFn = void (*)(const GfxRenderer& renderer, int x, int y);

// Draws the shared footer: separator line, brand icon + label (bottom-left),
// "<updatedPrefix> <lastUpdated>" centered (skipped if lastUpdated is empty),
// and identity text + battery icon (bottom-right). Landscape coordinates.
void drawFooter(const GfxRenderer& renderer, const ThemeMetrics& metrics, int pageWidth, int pageHeight,
                int sideMargin, BrandIconFn drawIcon, const char* brandLabel, const char* updatedPrefix,
                const char* lastUpdated, const char* identity);

// Sync the ESP32 internal clock via SNTP (only if not already set) and
// auto-detect the local UTC offset from the connection's IP (only if the
// clock offset is still the UTC+0 default). Safe to call on every poll; the
// RTC domain stays powered through timed deep sleep so this is a no-op after
// the first successful call. Requires WiFi to already be connected.
void syncClockAndTimezone();

// Format the current time (after syncClockAndTimezone) as "Jul 8 7:29 PM"
// honoring the clock format setting. Writes an empty string if the clock has
// never been synced, so callers can hide the "Updated" line rather than lie.
void formatUpdatedStamp(char* buf, size_t bufLen);

// Day of week (0 = Sunday) for an ISO "YYYY-MM-DD" date, via Sakamoto's algorithm.
int dayOfWeekSunday0(const char* isoDate);
extern const char* const DAY_ABBREV[7];  // "Sun".."Sat", indexed by dayOfWeekSunday0()

}  // namespace DashboardUI
