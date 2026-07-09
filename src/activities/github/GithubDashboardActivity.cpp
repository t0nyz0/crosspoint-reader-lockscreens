#include "GithubDashboardActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <ctime>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GithubDashboardSleep.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr size_t MAX_CELLS = 400;             // a year is 371 cells; guard against markup changes
constexpr size_t PARSE_TAIL_KEEP = 200;       // carry-over so tags/phrases can straddle chunk boundaries
// The heading is "<number>\n contributions\n in the last year" with the parts
// on separate lines, so anchor on the last part and walk backwards.
constexpr char CONTRIB_PHRASE[] = "in the last year";
constexpr char CONTRIB_WORD[] = "contributions";
const char* const MONTH_ABBREV[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Extract a quoted attribute value from tag; returns empty view semantics via out params.
bool findAttr(const std::string& buf, size_t tagStart, size_t tagEnd, const char* attr, size_t& valStart,
              size_t& valLen) {
  const size_t p = buf.find(attr, tagStart);
  if (p == std::string::npos || p >= tagEnd) return false;
  const size_t q1 = buf.find('"', p + strlen(attr));
  if (q1 == std::string::npos || q1 >= tagEnd) return false;
  const size_t q2 = buf.find('"', q1 + 1);
  if (q2 == std::string::npos || q2 > tagEnd) return false;
  valStart = q1 + 1;
  valLen = q2 - q1 - 1;
  return true;
}
}  // namespace

void GithubDashboardActivity::onEnter() {
  Activity::onEnter();

  if (SETTINGS.githubUsername[0] == '\0') {
    if (autoRefresh) {
      // Unattended wake with no username configured: leave the mode.
      exitDashboardMode();
      finish();
      return;
    }
    promptUsername();
    return;
  }

  beginUpdate();
}

void GithubDashboardActivity::onExit() {
  Activity::onExit();

  // Same heap-defrag pattern as ClockSyncActivity: reboot after a WiFi session.
  if (wifiUsed && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void GithubDashboardActivity::promptUsername() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_GITHUB_USERNAME),
                                              SETTINGS.githubUsername, sizeof(SETTINGS.githubUsername) - 1,
                                              InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          if (SETTINGS.githubUsername[0] == '\0') {
            finish();
          } else {
            // Back to the dashboard; re-arm the sleep window (auto-sleep is
            // disabled here, so a forgotten device must not stay awake).
            if (state == State::Showing) sleepAt = millis() + DISPLAY_GRACE_INTERACTIVE_MS;
            requestUpdate();
          }
          return;
        }
        const auto& kb = std::get<KeyboardResult>(result.data);
        strncpy(SETTINGS.githubUsername, kb.text.c_str(), sizeof(SETTINGS.githubUsername) - 1);
        SETTINGS.githubUsername[sizeof(SETTINGS.githubUsername) - 1] = '\0';
        SETTINGS.saveToFile();
        if (SETTINGS.githubUsername[0] == '\0') {
          finish();
          return;
        }
        beginUpdate();
      });
}

void GithubDashboardActivity::beginUpdate() {
  state = State::Connecting;
  errorMessage = nullptr;
  sleepAt = 0;

  if (WiFi.status() == WL_CONNECTED) {
    state = State::Fetching;
    if (!autoRefresh) requestUpdate();  // unattended wakes keep the previous frame on the panel
    return;
  }

  wifiUsed = true;
  if (autoRefresh) {
    // Unattended: connect directly to the last known network. The WiFi picker
    // UI would block forever with nobody in front of the device.
    startDirectWifiConnect();
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             finish();
                             return;
                           }
                           state = State::Fetching;
                           requestUpdate();
                         });
}

void GithubDashboardActivity::startDirectWifiConnect() {
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* cred = lastSsid.empty() ? nullptr : WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_ERR("GH", "No saved WiFi network for unattended refresh");
    state = State::Failed;
    errorMessage = tr(STR_GITHUB_WIFI_FAILED);
    return;  // loop() paints the error and re-arms the next poll
  }

  LOG_INF("GH", "Unattended refresh: connecting to %s", cred->ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());
  wifiConnectStart = millis();
}

void GithubDashboardActivity::loop() {
  switch (state) {
    case State::Connecting:
      if (!autoRefresh) return;  // interactive connect is driven by the WiFi sub-activity
      if (WiFi.status() == WL_CONNECTED) {
        state = State::Fetching;
        return;
      }
      if (millis() - wifiConnectStart >= WIFI_TIMEOUT_MS) {
        LOG_ERR("GH", "Unattended WiFi connect timed out");
        state = State::Failed;
        errorMessage = tr(STR_GITHUB_WIFI_FAILED);
        requestUpdateAndWait();
        // Keep the mode alive: try again at the next poll instead of draining
        // the battery waiting for input that isn't coming.
        goToSleepAndPoll();
      }
      return;

    case State::Fetching:
      // Paint the "Updating..." screen first, then block on the network.
      // On unattended wakes the panel keeps the previous dashboard instead.
      if (!autoRefresh) {
        requestUpdateAndWait();
      }
      runFetch();
      return;

    case State::Showing:
    case State::Failed:
      break;
  }

  if (autoRefresh) {
    // Unattended: paint whatever we ended up with and re-arm the next poll.
    requestUpdateAndWait();
    goToSleepAndPoll();
    return;
  }

  // Interactive: give the user a window to read/exit before the mode arms.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitDashboardMode();
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    sleepAt = 0;
    promptUsername();
    return;
  }
  if (state == State::Showing && sleepAt != 0 && millis() >= sleepAt) {
    goToSleepAndPoll();
  }
}

// Sync the ESP32 internal clock via SNTP while WiFi is up. The RTC domain
// stays powered through the dashboard's timed deep sleep, so once set the
// time survives between hourly polls and we only pay the SNTP wait once.
void GithubDashboardActivity::syncSystemClock() {
  time_t now = time(nullptr);
  if (now > 1735689600) return;  // already valid (>= 2025-01-01)
  if (WiFi.status() != WL_CONNECTED) return;

  LOG_INF("GH", "Syncing system clock via SNTP");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 50; i++) {  // up to ~5s
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) return;
    delay(100);
  }
  LOG_ERR("GH", "SNTP sync timed out");
}

// Detect the local UTC offset from the connection's IP and persist it to the
// clock settings, so the "Updated" stamp (and the reader clock features) show
// local time without manual setup. Re-checked on every poll so DST changes
// correct themselves within an hour.
void GithubDashboardActivity::autoDetectTimezone() {
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
    LOG_ERR("GH", "Timezone lookup failed, keeping current offset");
    return;
  }

  const uint8_t offsetQ = static_cast<uint8_t>(48 + offsetSeconds / 900);
  if (offsetQ != SETTINGS.clockUtcOffsetQ) {
    SETTINGS.clockUtcOffsetQ = offsetQ;
    SETTINGS.saveToFile();
  }
  LOG_INF("GH", "Timezone: UTC%+d min", offsetSeconds / 60);
}

void GithubDashboardActivity::captureUpdateTime() {
  time_t now = time(nullptr);
  if (now <= 1735689600) {
    lastUpdated[0] = '\0';  // clock never synced; hide the stamp rather than lie
    return;
  }
  // Apply the auto-detected UTC offset (quarter-hour steps, biased by 48).
  now += (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60;
  struct tm ti;
  gmtime_r(&now, &ti);
  int hour12 = ti.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  snprintf(lastUpdated, sizeof(lastUpdated), "%s %d %d:%02d %s", MONTH_ABBREV[ti.tm_mon], ti.tm_mday, hour12,
           ti.tm_min, ti.tm_hour < 12 ? "AM" : "PM");
}

void GithubDashboardActivity::runFetch() {
  cells.clear();
  cells.reserve(MAX_CELLS);
  totalContributions.clear();
  parseBuf.clear();
  memset(counts, 0, sizeof(counts));
  countsFound = false;

  syncSystemClock();
  autoDetectTimezone();

  char url[128];
  snprintf(url, sizeof(url), "https://github.com/users/%s/contributions", SETTINGS.githubUsername);

  LOG_INF("GH", "Fetching %s", url);
  const bool ok = HttpDownloader::fetchUrl(
      std::string(url), [this](const uint8_t* data, size_t len) { return feedHtml(data, len); });
  parseBuf.clear();
  parseBuf.shrink_to_fit();

  if (!ok || cells.size() < 30) {
    LOG_ERR("GH", "Fetch failed (ok=%d, cells=%u)", ok, (unsigned)cells.size());
    state = State::Failed;
    errorMessage = tr(STR_GITHUB_FETCH_FAILED);
  } else {
    std::sort(cells.begin(), cells.end(),
              [](const ContribCell& a, const ContribCell& b) { return strcmp(a.date, b.date) < 0; });
    computeStats();
    captureUpdateTime();
    LOG_INF("GH", "Parsed %u days, total %u, streak %d/%d, max %u", (unsigned)cells.size(), (unsigned)statTotal,
            statCurrentStreak, statLongestStreak, (unsigned)statMostInDay);
    state = State::Showing;
  }

  requestUpdateAndWait();

  if (autoRefresh) {
    goToSleepAndPoll();
  } else if (state == State::Showing) {
    sleepAt = millis() + DISPLAY_GRACE_INTERACTIVE_MS;
  }
}

bool GithubDashboardActivity::feedHtml(const uint8_t* data, size_t len) {
  parseBuf.append(reinterpret_cast<const char*>(data), len);

  size_t consumed = 0;
  while (true) {
    const size_t td = parseBuf.find("<td", consumed);
    const size_t tt = parseBuf.find("<tool-tip", consumed);
    const size_t next = std::min(td, tt);
    if (next == std::string::npos) {
      consumed = parseBuf.size() > PARSE_TAIL_KEEP ? parseBuf.size() - PARSE_TAIL_KEEP : 0;
      break;
    }
    const size_t end = parseBuf.find('>', next);
    if (end == std::string::npos) {
      consumed = next;  // incomplete tag, wait for the next chunk
      break;
    }

    if (next == td) {
      // Day cell: date + level
      size_t dStart = 0, dLen = 0, lStart = 0, lLen = 0;
      if (cells.size() < MAX_CELLS && findAttr(parseBuf, td, end, "data-date=", dStart, dLen) && dLen == 10 &&
          findAttr(parseBuf, td, end, "data-level=", lStart, lLen) && lLen >= 1) {
        ContribCell cell;
        memcpy(cell.date, parseBuf.data() + dStart, 10);
        cell.date[10] = '\0';
        const char lvl = parseBuf[lStart];
        cell.level = (lvl >= '0' && lvl <= '4') ? lvl - '0' : 0;
        cells.push_back(cell);
      }
      consumed = end + 1;
    } else {
      // Tool-tip: per-day count text ("5 contributions on ..." / "No contributions on ..."),
      // linked to a cell via for="contribution-day-component-<row>-<col>".
      const size_t textEnd = parseBuf.find('<', end + 1);
      if (textEnd == std::string::npos) {
        consumed = next;  // count text not fully buffered yet
        break;
      }
      const size_t f = parseBuf.find("contribution-day-component-", next);
      if (f != std::string::npos && f < end) {
        int row = -1, col = -1;
        if (sscanf(parseBuf.c_str() + f, "contribution-day-component-%d-%d", &row, &col) == 2 && row >= 0 &&
            row < 7 && col >= 0) {
          const size_t slot = static_cast<size_t>(col) * 7 + row;
          size_t t = end + 1;
          while (t < textEnd && isspace(static_cast<unsigned char>(parseBuf[t]))) t++;
          if (slot < MAX_SLOTS && t < textEnd && isdigit(static_cast<unsigned char>(parseBuf[t]))) {
            const long n = atol(parseBuf.c_str() + t);
            counts[slot] = n < 0 ? 0 : (n > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(n));
            countsFound = true;
          }
          // "No contributions ..." leaves the slot at 0
        }
      }
      consumed = textEnd;
    }
  }

  if (totalContributions.empty()) {
    scanForTotal();
  }
  if (consumed > 0) {
    parseBuf.erase(0, consumed);
  }
  return true;
}

bool GithubDashboardActivity::cellContributed(size_t i, int offset) const {
  if (!countsFound) return cells[i].level > 0;
  const size_t slot = offset + i;
  return slot < MAX_SLOTS && counts[slot] > 0;
}

void GithubDashboardActivity::computeStats() {
  statTotal = 0;
  statMostInDay = 0;
  statLongestStreak = 0;
  statCurrentStreak = 0;
  if (cells.empty()) return;

  const int offset = dayOfWeekSunday0(cells.front().date);

  int streak = 0;
  for (size_t i = 0; i < cells.size(); i++) {
    if (countsFound) {
      const size_t slot = offset + i;
      const uint16_t c = slot < MAX_SLOTS ? counts[slot] : 0;
      statTotal += c;
      if (c > statMostInDay) statMostInDay = c;
    }
    if (cellContributed(i, offset)) {
      streak++;
      if (streak > statLongestStreak) statLongestStreak = streak;
    } else {
      streak = 0;
    }
  }

  // Current streak: today without contributions (yet) doesn't break it.
  int j = static_cast<int>(cells.size()) - 1;
  if (j >= 0 && !cellContributed(j, offset)) j--;
  while (j >= 0 && cellContributed(j, offset)) {
    statCurrentStreak++;
    j--;
  }

  if (!countsFound) {
    // Fall back to the heading total ("3,640") when tool-tips were missing.
    for (const char c : totalContributions) {
      if (isdigit(static_cast<unsigned char>(c))) statTotal = statTotal * 10 + (c - '0');
    }
  }
}

void GithubDashboardActivity::scanForTotal() {
  const size_t p = parseBuf.find(CONTRIB_PHRASE);
  if (p == std::string::npos) return;

  // Walk back: whitespace, the word "contributions", whitespace, then the
  // "3,640"-style number.
  size_t i = p;
  while (i > 0 && isspace(static_cast<unsigned char>(parseBuf[i - 1]))) i--;
  const size_t wordLen = strlen(CONTRIB_WORD);
  if (i < wordLen || parseBuf.compare(i - wordLen, wordLen, CONTRIB_WORD) != 0) return;
  i -= wordLen;
  while (i > 0 && isspace(static_cast<unsigned char>(parseBuf[i - 1]))) i--;
  const size_t numEnd = i;
  while (i > 0 && (isdigit(static_cast<unsigned char>(parseBuf[i - 1])) || parseBuf[i - 1] == ',')) i--;
  if (numEnd > i) {
    totalContributions = parseBuf.substr(i, numEnd - i);
  }
}

void GithubDashboardActivity::goToSleepAndPoll() {
  APP_STATE.githubDashboardMode = true;
  APP_STATE.saveToFile();
  LOG_INF("GH", "Dashboard armed, sleeping for %u s", (unsigned)POLL_INTERVAL_S);
  enterGithubDashboardSleep(POLL_INTERVAL_S);  // does not return
}

void GithubDashboardActivity::exitDashboardMode() {
  if (APP_STATE.githubDashboardMode) {
    APP_STATE.githubDashboardMode = false;
    APP_STATE.saveToFile();
  }
}

int GithubDashboardActivity::dayOfWeekSunday0(const char* isoDate) {
  // Sakamoto's algorithm, 0 = Sunday
  int y = atoi(isoDate);
  const int m = atoi(isoDate + 5);
  const int d = atoi(isoDate + 8);
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

void GithubDashboardActivity::render(RenderLock&&) {
  switch (state) {
    case State::Connecting:
    case State::Fetching:
      renderMessage(tr(STR_GITHUB_UPDATING));
      break;
    case State::Failed:
      renderMessage(errorMessage ? errorMessage : tr(STR_GITHUB_FETCH_FAILED));
      break;
    case State::Showing:
      renderDashboard();
      break;
  }
}

void GithubDashboardActivity::renderMessage(const char* message) const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  char title[64];
  snprintf(title, sizeof(title), "@%s", SETTINGS.githubUsername);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, title, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 5, message);

  if (!autoRefresh && state == State::Failed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GITHUB_CHANGE_USER), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

// Small 4x4 contribution-grid glyph for the footer branding (drawIcon assumes
// portrait orientation, so the landscape dashboard draws its own mark).
namespace {
void drawGridMark(const GfxRenderer& renderer, int x, int y, int cell, int gap) {
  static constexpr uint8_t pattern[4] = {0b1011, 0b0110, 0b1101, 0b1011};  // 1 = filled
  const int pitch = cell + gap;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (pattern[r] & (1 << (3 - c))) {
        renderer.fillRect(x + c * pitch, y + r * pitch, cell, cell);
      } else {
        renderer.drawRect(x + c * pitch, y + r * pitch, cell, cell);
      }
    }
  }
}
}  // namespace

// 5x7 dot-matrix glyphs for the big stat numbers (digits, '.', 'k').
// Each glyph is 7 rows of 5 bits, MSB = leftmost column.
namespace {
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
    {'k', {0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010}, 5},
};

const BigGlyph* findBigGlyph(char c) {
  for (const auto& g : BIG_GLYPHS) {
    if (g.ch == c) return &g;
  }
  return nullptr;
}

void formatCompact(uint32_t n, char* out, size_t outLen) {
  if (n >= 10000) {
    snprintf(out, outLen, "%uk", (unsigned)(n / 1000));
  } else if (n >= 1000) {
    snprintf(out, outLen, "%u.%uk", (unsigned)(n / 1000), (unsigned)((n % 1000) / 100));
  } else {
    snprintf(out, outLen, "%u", (unsigned)n);
  }
}
}  // namespace

int GithubDashboardActivity::bigTextWidth(const char* text, int dot) {
  int w = 0;
  for (const char* p = text; *p; p++) {
    const BigGlyph* g = findBigGlyph(*p);
    w += ((g ? g->width : 5) + 1) * dot;
  }
  return w > 0 ? w - dot : 0;  // drop trailing inter-glyph space
}

void GithubDashboardActivity::drawBigText(int x, int y, const char* text, int dot) const {
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

void GithubDashboardActivity::renderDashboard() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // The dashboard is a landscape screen; restore the caller's orientation when done.
  const auto origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
  const auto pageWidth = renderer.getScreenWidth();    // 800
  const auto pageHeight = renderer.getScreenHeight();  // 480
  constexpr int sideMargin = 40;

  renderer.clearScreen();

  // --- Top left: big compact total + caption (TRMNL style) ---
  char hero[16];
  formatCompact(statTotal, hero, sizeof(hero));
  renderer.fillRectDither(sideMargin - 16, 42, 8, 74, Color::DarkGray);  // accent bar
  drawBigText(sideMargin, 42, hero, 10);
  renderer.drawText(UI_10_FONT_ID, sideMargin, 130, tr(STR_GITHUB_CONTRIB_CAPTION));

  // --- Top right: 2x2 stat grid ---
  struct StatEntry {
    char value[16];
    const char* label;
  };
  StatEntry stats[4];
  snprintf(stats[0].value, sizeof(stats[0].value), "%d", statLongestStreak);
  stats[0].label = tr(STR_GITHUB_LONGEST_STREAK);
  snprintf(stats[1].value, sizeof(stats[1].value), "%d", statCurrentStreak);
  stats[1].label = tr(STR_GITHUB_CURRENT_STREAK);
  if (countsFound) {
    snprintf(stats[2].value, sizeof(stats[2].value), "%u", (unsigned)statMostInDay);
    const float avg = cells.empty() ? 0.0f : static_cast<float>(statTotal) / static_cast<float>(cells.size());
    snprintf(stats[3].value, sizeof(stats[3].value), "%.2f", avg);
  } else {
    snprintf(stats[2].value, sizeof(stats[2].value), "-");
    snprintf(stats[3].value, sizeof(stats[3].value), "-");
  }
  stats[2].label = tr(STR_GITHUB_MOST_IN_DAY);
  stats[3].label = tr(STR_GITHUB_AVG_PER_DAY);

  for (int s = 0; s < 4; s++) {
    const int colX = 430 + (s % 2) * 190;
    const int rowY = 42 + (s / 2) * 78;
    renderer.fillRectDither(colX - 16, rowY, 8, 58, Color::DarkGray);  // accent bar
    drawBigText(colX, rowY, stats[s].value, 5);
    renderer.drawText(UI_10_FONT_ID, colX, rowY + 40, stats[s].label);
  }

  renderer.fillRect(sideMargin, 196, pageWidth - 2 * sideMargin, 1);

  // --- Heatmap: weeks as columns, Sunday-first rows, GitHub-style grid ---
  if (!cells.empty()) {
    const int offset = dayOfWeekSunday0(cells.front().date);
    const int weeks = (offset + static_cast<int>(cells.size()) + 6) / 7;
    constexpr int cellW = 11;
    constexpr int pitchX = 13;
    constexpr int cellH = 14;
    constexpr int pitchY = 17;
    const int gridW = weeks * pitchX;
    const int labelGutter = 34;  // room for Mon/Wed/Fri labels
    const int x0 = std::max(sideMargin + labelGutter, (pageWidth - gridW + labelGutter) / 2);
    const int y0 = 248;

    // Weekday labels like GitHub (rows are Sunday-first)
    renderer.drawText(SMALL_FONT_ID, x0 - labelGutter, y0 + 1 * pitchY - 1, "Mon");
    renderer.drawText(SMALL_FONT_ID, x0 - labelGutter, y0 + 3 * pitchY - 1, "Wed");
    renderer.drawText(SMALL_FONT_ID, x0 - labelGutter, y0 + 5 * pitchY - 1, "Fri");

    // Month labels above the columns where a new month starts on a week boundary
    int lastLabelCol = -100;
    for (size_t i = 0; i < cells.size(); i++) {
      const int slot = offset + static_cast<int>(i);
      if (slot % 7 != 0) continue;  // only the top (Sunday) row starts a column
      const int month = atoi(cells[i].date + 5);
      const int day = atoi(cells[i].date + 8);
      if (day > 7) continue;  // not the first week of the month
      const int col = slot / 7;
      if (col - lastLabelCol < 3) continue;  // avoid overlapping labels
      if (month >= 1 && month <= 12) {
        renderer.drawText(SMALL_FONT_ID, x0 + col * pitchX, y0 - 20, MONTH_ABBREV[month - 1]);
        lastLabelCol = col;
      }
    }

    // Crisp level ramp for 1-bit e-ink: dot, outline, gray fill, solid black.
    for (size_t i = 0; i < cells.size(); i++) {
      const int slot = offset + static_cast<int>(i);
      const int x = x0 + (slot / 7) * pitchX;
      const int y = y0 + (slot % 7) * pitchY;
      switch (cells[i].level) {
        case 0:
          break;  // blank — dots for every empty day read as noise on e-ink
        case 1:
          renderer.drawRoundedRect(x, y, cellW, cellH, 1, 2, true);
          break;
        case 2:
          renderer.drawRoundedRect(x, y, cellW, cellH, 1, 2, true);
          renderer.fillRectDither(x + 2, y + 2, cellW - 4, cellH - 4, Color::DarkGray);
          break;
        default:
          renderer.fillRoundedRect(x, y, cellW, cellH, 2, Color::Black);
          break;
      }
    }
  }

  // --- Footer bar: GitHub branding, updated time, username + battery ---
  const int sepY = pageHeight - 54;
  renderer.fillRect(sideMargin, sepY, pageWidth - 2 * sideMargin, 1);
  const int footerTextY = sepY + 18;

  drawGridMark(renderer, sideMargin, sepY + 15, 5, 2);  // 26px mini contribution grid
  renderer.drawText(UI_10_FONT_ID, sideMargin + 38, footerTextY, "GitHub", true, EpdFontFamily::BOLD);

  if (lastUpdated[0] != '\0') {
    char line[40];
    snprintf(line, sizeof(line), "%s %s", tr(STR_GITHUB_UPDATED), lastUpdated);
    renderer.drawCenteredText(UI_10_FONT_ID, footerTextY, line);
  }

  const int userW = renderer.getTextWidth(UI_10_FONT_ID, SETTINGS.githubUsername);
  const int battX = pageWidth - sideMargin - metrics.batteryWidth;
  renderer.drawText(UI_10_FONT_ID, battX - 10 - userW, footerTextY, SETTINGS.githubUsername);
  GUI.drawBatteryLeft(renderer, Rect{battX, footerTextY + 2, metrics.batteryWidth, metrics.batteryHeight}, false);

  // Full refresh: this frame stays on the panel for the whole sleep hour.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  renderer.setOrientation(origOrientation);
}
