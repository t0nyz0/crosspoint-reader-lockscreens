#include "DashboardUI.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace DashboardUI {

namespace {
const char* const MONTH_ABBREV[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
}  // namespace

const char* const DAY_ABBREV[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

int dayOfWeekSunday0(const char* isoDate) {
  int y = atoi(isoDate);
  const int m = atoi(isoDate + 5);
  const int d = atoi(isoDate + 8);
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

namespace {
// 5x7 dot-matrix glyphs for the big stat numbers. Each glyph is 7 rows of 5
// bits, MSB = leftmost column.
struct BigGlyph {
  char ch;
  uint8_t rows[7];
  uint8_t width;  // in dot columns
};
constexpr BigGlyph BIG_GLYPHS[] = {
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}, 5},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, 5},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}, 5},
    {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}, 5},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}, 5},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}, 5},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}, 5},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}, 5},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}, 5},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}, 5},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11000, 0b11000}, 2},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}, 5},
    {'k', {0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010}, 5},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}, 5},
    {'%', {0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011}, 5},
};

const BigGlyph* findBigGlyph(char c) {
  for (const auto& g : BIG_GLYPHS) {
    if (g.ch == c) return &g;
  }
  return nullptr;
}
}  // namespace

void formatCompact(uint32_t n, char* out, size_t outLen) {
  if (n >= 10000) {
    snprintf(out, outLen, "%uk", (unsigned)(n / 1000));
  } else if (n >= 1000) {
    snprintf(out, outLen, "%u.%uk", (unsigned)(n / 1000), (unsigned)((n % 1000) / 100));
  } else {
    snprintf(out, outLen, "%u", (unsigned)n);
  }
}

int bigTextWidth(const char* text, int dot) {
  int w = 0;
  for (const char* p = text; *p; p++) {
    const BigGlyph* g = findBigGlyph(*p);
    w += ((g ? g->width : 5) + 1) * dot;
  }
  return w > 0 ? w - dot : 0;  // drop trailing inter-glyph space
}

void drawBigText(const GfxRenderer& renderer, int x, int y, const char* text, int dot) {
  for (const char* p = text; *p; p++) {
    const BigGlyph* g = findBigGlyph(*p);
    if (!g) {
      x += 6 * dot;
      continue;
    }
    for (int r = 0; r < 7; r++) {
      for (int c = 0; c < 5; c++) {
        if (g->rows[r] & (1 << (4 - c))) {
          renderer.fillRect(x + c * dot, y + r * dot, dot, dot);
        }
      }
    }
    x += (g->width + 1) * dot;
  }
}

void drawStatTile(const GfxRenderer& renderer, int x, int y, const char* value, const char* label) {
  renderer.fillRectDither(x - 16, y, 8, 58, Color::DarkGray);  // accent bar
  drawBigText(renderer, x, y, value, 5);
  renderer.drawText(UI_10_FONT_ID, x, y + 40, label);
}

void drawFooter(const GfxRenderer& renderer, const ThemeMetrics& metrics, int pageWidth, int pageHeight,
                int sideMargin, BrandIconFn drawIcon, const char* brandLabel, const char* updatedPrefix,
                const char* lastUpdated, const char* identity) {
  // In portrait there isn't room for brand + updated-time + identity + battery
  // on one line, so lay the footer out in two rows: brand/battery on top, the
  // updated stamp and identity below. Landscape keeps the single-line layout.
  const bool portrait = pageWidth < 600;
  const int battX = pageWidth - sideMargin - metrics.batteryWidth;

  if (portrait) {
    const int sepY = pageHeight - 72;
    renderer.fillRect(sideMargin, sepY, pageWidth - 2 * sideMargin, 1);
    const int row1Y = sepY + 14;
    const int row2Y = sepY + 44;

    // Row 1: brand (left), battery + % (right)
    if (drawIcon) drawIcon(renderer, sideMargin, sepY + 11);
    renderer.drawText(UI_10_FONT_ID, sideMargin + 38, row1Y, brandLabel, true, EpdFontFamily::BOLD);
    GUI.drawBatteryRight(renderer, Rect{battX, row1Y + 2, metrics.batteryWidth, metrics.batteryHeight}, true);

    // Row 2: updated stamp (left), identity (right)
    if (lastUpdated && lastUpdated[0] != '\0') {
      char line[48];
      snprintf(line, sizeof(line), "%s %s", updatedPrefix, lastUpdated);
      renderer.drawText(SMALL_FONT_ID, sideMargin, row2Y, line);
    }
    if (identity && identity[0] != '\0') {
      const int identityW = renderer.getTextWidth(SMALL_FONT_ID, identity);
      renderer.drawText(SMALL_FONT_ID, pageWidth - sideMargin - identityW, row2Y, identity);
    }
    return;
  }

  const int sepY = pageHeight - 54;
  renderer.fillRect(sideMargin, sepY, pageWidth - 2 * sideMargin, 1);
  const int footerTextY = sepY + 18;

  if (drawIcon) drawIcon(renderer, sideMargin, sepY + 15);
  renderer.drawText(UI_10_FONT_ID, sideMargin + 38, footerTextY, brandLabel, true, EpdFontFamily::BOLD);

  if (lastUpdated && lastUpdated[0] != '\0') {
    char line[48];
    snprintf(line, sizeof(line), "%s %s", updatedPrefix, lastUpdated);
    renderer.drawCenteredText(UI_10_FONT_ID, footerTextY, line);
  }

  // Battery icon + explicit "NN%" (a bare icon glyph is hard to read at a
  // glance on e-ink) -- drawBatteryRight puts the icon on the right and the
  // percentage just to its left, so the identity text only needs to clear
  // the percentage label, not guess where the icon starts.
  char pctText[8];
  snprintf(pctText, sizeof(pctText), "%u%%", static_cast<unsigned>(powerManager.getBatteryPercentage()));
  const int pctTextW = renderer.getTextWidth(SMALL_FONT_ID, pctText);
  const int identityRightEdge = battX - pctTextW - 14;

  if (identity && identity[0] != '\0') {
    const int identityW = renderer.getTextWidth(UI_10_FONT_ID, identity);
    renderer.drawText(UI_10_FONT_ID, identityRightEdge - identityW, footerTextY, identity);
  }
  GUI.drawBatteryRight(renderer, Rect{battX, footerTextY + 2, metrics.batteryWidth, metrics.batteryHeight}, true);
}

void syncClockAndTimezone() {
  if (WiFi.status() != WL_CONNECTED) return;

  time_t now = time(nullptr);
  if (now <= 1735689600) {  // not yet valid (< 2025-01-01)
    LOG_INF("DASH", "Syncing system clock via SNTP");
    configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
    for (int i = 0; i < 50; i++) {  // up to ~5s
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) break;
      delay(100);
    }
  }

  // Re-detect on EVERY poll (not once): the provider's offset already includes
  // the current DST state, so refreshing each poll keeps the clock correct
  // across the twice-yearly DST transitions instead of freezing at whatever
  // offset happened to be detected first. A manual offset change turns
  // clockTzAutoDetect off so the user's explicit choice is never overwritten.
  if (!SETTINGS.clockTzAutoDetect) return;

  int offsetSeconds = 0;
  bool found = false;

  // Primary: ip-api.com (free tier is plain http; offset includes DST)
  std::string body;
  if (HttpDownloader::fetchUrl("http://ip-api.com/json/?fields=status,offset", body)) {
    const size_t p = body.find("\"offset\":");
    if (p != std::string::npos && body.find("\"success\"") != std::string::npos) {
      offsetSeconds = atoi(body.c_str() + p + 9);
      found = true;
    }
  }

  // Fallback: worldtimeapi.org ("utc_offset":"-05:00")
  if (!found) {
    body.clear();
    if (HttpDownloader::fetchUrl("https://worldtimeapi.org/api/ip", body)) {
      const size_t p = body.find("\"utc_offset\":\"");
      if (p != std::string::npos && p + 20 <= body.size()) {
        const char* s = body.c_str() + p + 14;  // e.g. "-05:00"
        if ((s[0] == '+' || s[0] == '-') && isdigit(static_cast<unsigned char>(s[1]))) {
          const int sign = s[0] == '-' ? -1 : 1;
          offsetSeconds = sign * (atoi(s + 1) * 3600 + atoi(s + 4) * 60);
          found = true;
        }
      }
    }
  }

  if (!found || offsetSeconds < -14 * 3600 || offsetSeconds > 14 * 3600) {
    LOG_ERR("DASH", "Timezone lookup failed, keeping current offset");
    return;
  }

  const uint8_t detectedQ = static_cast<uint8_t>(48 + offsetSeconds / 900);
  if (detectedQ != SETTINGS.clockUtcOffsetQ) {  // only write to SD when it actually changes
    SETTINGS.clockUtcOffsetQ = detectedQ;
    SETTINGS.saveToFile();
    LOG_INF("DASH", "Timezone updated: UTC%+d min", offsetSeconds / 60);
  }
}

void formatUpdatedStamp(char* buf, size_t bufLen) {
  time_t now = time(nullptr);
  if (now <= 1735689600) {
    buf[0] = '\0';  // clock never synced; hide the stamp rather than lie
    return;
  }
  now += (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60;
  struct tm ti;
  gmtime_r(&now, &ti);
  int hour12 = ti.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  snprintf(buf, bufLen, "%s %d %d:%02d %s", MONTH_ABBREV[ti.tm_mon], ti.tm_mday, hour12, ti.tm_min,
           ti.tm_hour < 12 ? "AM" : "PM");
}

namespace {
constexpr const char* COMPASS[16] = {"N",  "NNE", "NE", "ENE", "E",  "ESE", "SE", "SSE",
                                     "S",  "SSW", "SW", "WSW", "W",  "WNW", "NW", "NNW"};
}  // namespace

const char* compassDirection(int degrees) {
  const int idx = ((degrees % 360 + 360) % 360 + 11) * 16 / 360 % 16;
  return COMPASS[idx];
}

namespace {
// Filled circle via the rounded-rect trick (corner radius = half the side).
void fillCircle(const GfxRenderer& r, int cx, int cy, int rad) {
  if (rad < 1) rad = 1;
  r.fillRoundedRect(cx - rad, cy - rad, rad * 2, rad * 2, rad, Color::Black);
}

// Sun: filled disc + 8 rays at 45-degree steps, all scaled to discR.
void drawSun(const GfxRenderer& r, int cx, int cy, int discR) {
  fillCircle(r, cx, cy, discR);
  const int gap = discR * 45 / 100 + 2;
  const int len = discR * 75 / 100 + 2;
  const int rayW = discR >= 14 ? 3 : 2;
  const int inner = discR + gap;
  const int outer = inner + len;
  for (int a = 0; a < 8; a++) {
    const float ang = static_cast<float>(a) * 3.14159265f / 4.0f;
    const float s = std::sin(ang), c = std::cos(ang);
    r.drawLine(cx + static_cast<int>(s * inner), cy - static_cast<int>(c * inner),
               cx + static_cast<int>(s * outer), cy - static_cast<int>(c * outer), rayW, true);
  }
}

// Classic cloud silhouette (bumpy top, flat bottom) filling the box
// [x, x+w) x [y, y+h). A flat base slab plus three overlapping lumps, so the
// union reads unmistakably as a cloud even as a solid 1-bit shape (rather than
// the single oval it used to be).
void drawCloud(const GfxRenderer& r, int x, int y, int w, int h) {
  // Rounded base slab (rounded bottom corners keep it from looking boxy) plus
  // three overlapping lumps -- big center, lower/wider sides that bulge out.
  const int baseTop = y + h * 46 / 100;
  r.fillRoundedRect(x + w * 12 / 100, baseTop, w * 76 / 100, (y + h) - baseTop, h * 22 / 100, Color::Black);
  fillCircle(r, x + w * 50 / 100, y + h * 34 / 100, h * 32 / 100);  // center (tallest)
  fillCircle(r, x + w * 28 / 100, y + h * 54 / 100, h * 26 / 100);  // left
  fillCircle(r, x + w * 72 / 100, y + h * 52 / 100, h * 27 / 100);  // right
}
}  // namespace

void drawWeatherIcon(const GfxRenderer& renderer, WxCategory category, int x, int y, int size) {
  if (category == WxCategory::Clear) {
    drawSun(renderer, x + size / 2, y + size / 2, size * 30 / 100);
    return;
  }
  if (category == WxCategory::PartlyCloudy) {
    // Small sun peeking from the top-left, cloud overlapping the lower-right.
    drawSun(renderer, x + size * 34 / 100, y + size * 30 / 100, size * 18 / 100);
    drawCloud(renderer, x + size * 18 / 100, y + size * 34 / 100, size * 82 / 100, size * 58 / 100);
    return;
  }

  // Every other condition is a cloud with something beneath it. Reserve the
  // lower part of the box for the precipitation/effect marks.
  const int cloudH = size * 66 / 100;
  drawCloud(renderer, x, y, size, cloudH);
  const int precipTop = y + cloudH + std::max(2, size / 20);

  switch (category) {
    case WxCategory::Rain:
    case WxCategory::Drizzle: {
      const int streak = category == WxCategory::Rain ? size * 20 / 100 : size * 12 / 100;
      for (int i = 0; i < 3; i++) {
        const int px = x + size * 30 / 100 + i * size * 20 / 100;
        renderer.drawLine(px + streak / 3, precipTop, px - streak / 3, precipTop + streak, 2, true);
      }
      break;
    }
    case WxCategory::Snow: {
      const int flakeR = size * 8 / 100;
      for (int i = 0; i < 3; i++) {
        const int px = x + size * 30 / 100 + i * size * 20 / 100;
        const int py = precipTop + flakeR;
        if (flakeR >= 4) {
          // 3-spoke asterisk flake
          for (int a = 0; a < 3; a++) {
            const float ang = static_cast<float>(a) * 3.14159265f / 3.0f;
            const float s = std::sin(ang), c = std::cos(ang);
            renderer.drawLine(px - static_cast<int>(c * flakeR), py - static_cast<int>(s * flakeR),
                              px + static_cast<int>(c * flakeR), py + static_cast<int>(s * flakeR), 1, true);
          }
        } else {
          renderer.fillRect(px - 1, py - 1, 3, 3);
        }
      }
      break;
    }
    case WxCategory::Storm: {
      // Filled lightning bolt (zigzag) centered under the cloud.
      const int cx = x + size / 2;
      const int t = precipTop;
      const int b = precipTop + size * 26 / 100;
      const int m = (t + b) / 2;
      const int wdt = size * 10 / 100 + 2;
      renderer.drawLine(cx + wdt / 2, t, cx - wdt, m, 3, true);
      renderer.drawLine(cx - wdt, m, cx + wdt / 3, m, 3, true);
      renderer.drawLine(cx + wdt / 3, m, cx - wdt, b, 3, true);
      break;
    }
    case WxCategory::Fog: {
      const int n = 3;
      for (int i = 0; i < n; i++) {
        const int inset = (i % 2) * (size * 12 / 100);
        renderer.fillRect(x + size * 12 / 100 + inset, precipTop + i * std::max(3, size / 14),
                          size * 76 / 100 - inset, std::max(2, size / 28));
      }
      break;
    }
    default:
      break;
  }
}

void drawWindDial(const GfxRenderer& renderer, int cx, int cy, int radius, int windDegrees) {
  // Outline circle approximated via a fully-rounded square bounding box.
  renderer.drawRoundedRect(cx - radius, cy - radius, radius * 2, radius * 2, 1, radius, true);

  // Needle: 0 deg = straight up (North), rotates clockwise with wind direction.
  const float rad = static_cast<float>(windDegrees) * 3.14159265f / 180.0f;
  const int tipX = cx + static_cast<int>(std::round(std::sin(rad) * (radius - 3)));
  const int tipY = cy - static_cast<int>(std::round(std::cos(rad) * (radius - 3)));
  renderer.drawLine(cx, cy, tipX, tipY, 2, true);
  renderer.fillRoundedRect(cx - 2, cy - 2, 4, 4, 2, Color::Black);
}

}  // namespace DashboardUI
