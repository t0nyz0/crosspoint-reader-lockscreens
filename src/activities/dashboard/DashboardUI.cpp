#include "DashboardUI.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

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

void drawFooter(const GfxRenderer& renderer, const ThemeMetrics& metrics, int pageWidth, int pageHeight,
                int sideMargin, BrandIconFn drawIcon, const char* brandLabel, const char* updatedPrefix,
                const char* lastUpdated, const char* identity) {
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
  const int battX = pageWidth - sideMargin - metrics.batteryWidth;
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

  if (SETTINGS.clockUtcOffsetQ != 48) return;  // already detected or manually set

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

  SETTINGS.clockUtcOffsetQ = static_cast<uint8_t>(48 + offsetSeconds / 900);
  SETTINGS.saveToFile();
  LOG_INF("DASH", "Timezone: UTC%+d min", offsetSeconds / 60);
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

void drawWeatherIcon(const GfxRenderer& renderer, WxCategory category, int x, int y, int size) {
  const int r = size / 2;

  if (category == WxCategory::Clear || category == WxCategory::PartlyCloudy) {
    // Sun: filled disc + 8 rays (cardinal + diagonal), all scaled with size
    // instead of fixed pixel stubs -- otherwise the rays stay hairline-thin
    // no matter how big the disc gets.
    const int rayLen = size / 5 > 3 ? size / 5 : 3;
    const int rayGap = size / 12 > 1 ? size / 12 : 1;
    const int rayWidth = size / 20 > 1 ? size / 20 : 1;
    const int cx = x + r / 2;
    const int cy = y + r / 2;

    renderer.fillRoundedRect(x, y, r, r, r / 2, Color::Black);

    // Cardinal rays
    renderer.drawLine(cx, y - rayGap - rayLen, cx, y - rayGap, rayWidth, true);
    renderer.drawLine(cx, y + r + rayGap, cx, y + r + rayGap + rayLen, rayWidth, true);
    renderer.drawLine(x - rayGap - rayLen, cy, x - rayGap, cy, rayWidth, true);
    renderer.drawLine(x + r + rayGap, cy, x + r + rayGap + rayLen, cy, rayWidth, true);

    // Diagonal rays
    constexpr float kDiag = 0.7071f;  // cos/sin(45 deg)
    const float innerR = static_cast<float>(r) / 2.0f + rayGap;
    const float outerR = innerR + rayLen;
    for (int sx = -1; sx <= 1; sx += 2) {
      for (int sy = -1; sy <= 1; sy += 2) {
        const int x1 = cx + static_cast<int>(sx * kDiag * innerR);
        const int y1 = cy + static_cast<int>(sy * kDiag * innerR);
        const int x2 = cx + static_cast<int>(sx * kDiag * outerR);
        const int y2 = cy + static_cast<int>(sy * kDiag * outerR);
        renderer.drawLine(x1, y1, x2, y2, rayWidth, true);
      }
    }
  }
  if (category == WxCategory::Clear) return;

  // Cloud body for everything else (partly-cloudy layers it under the sun)
  const int cx = category == WxCategory::PartlyCloudy ? x + r / 2 : x;
  const int cy = category == WxCategory::PartlyCloudy ? y + r / 2 : y;
  const int cw = size;
  const int ch = size * 2 / 3;
  renderer.fillRoundedRect(cx, cy, cw, ch, ch / 2, Color::Black);

  switch (category) {
    case WxCategory::Rain:
    case WxCategory::Drizzle:
      for (int i = 0; i < 3; i++) {
        const int dx = cx + 4 + i * (cw - 8) / 2;
        renderer.drawLine(dx, cy + ch + 2, dx - 2, cy + ch + 8, 2, true);
      }
      break;
    case WxCategory::Snow:
      for (int i = 0; i < 3; i++) {
        const int dx = cx + 4 + i * (cw - 8) / 2;
        renderer.fillRect(dx - 1, cy + ch + 3, 3, 3);
      }
      break;
    case WxCategory::Storm:
      renderer.drawLine(cx + cw / 2 + 2, cy + ch, cx + cw / 2 - 3, cy + ch + 5, 2, true);
      renderer.drawLine(cx + cw / 2 - 3, cy + ch + 5, cx + cw / 2 + 1, cy + ch + 5, 2, true);
      renderer.drawLine(cx + cw / 2 + 1, cy + ch + 5, cx + cw / 2 - 4, cy + ch + 10, 2, true);
      break;
    case WxCategory::Fog:
      for (int i = 0; i < 3; i++) {
        renderer.fillRect(cx, cy + ch + 2 + i * 4, cw, 2);
      }
      break;
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
