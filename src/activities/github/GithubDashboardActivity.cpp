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
#include "DashboardSleep.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/dashboard/DashboardUI.h"
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
  // On an unattended timer-wake boot nothing has loaded the credential store
  // yet (the WiFi picker normally does it) — without this the store is empty
  // and every hourly reconnect fails. SD access needs the SPI render lock.
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* cred = lastSsid.empty() ? nullptr : WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_ERR("GH", "No saved WiFi network for unattended refresh");
    state = State::Failed;
    errorMessage = tr(STR_DASHBOARD_WIFI_FAILED);
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
        errorMessage = tr(STR_DASHBOARD_WIFI_FAILED);
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

void GithubDashboardActivity::runFetch() {
  cells.clear();
  cells.reserve(MAX_CELLS);
  totalContributions.clear();
  parseBuf.clear();
  memset(counts, 0, sizeof(counts));
  countsFound = false;

  DashboardUI::syncClockAndTimezone();

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
    DashboardUI::formatUpdatedStamp(lastUpdated, sizeof(lastUpdated));
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

  const int offset = DashboardUI::dayOfWeekSunday0(cells.front().date);

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
  APP_STATE.activeDashboardMode = CrossPointState::DASHBOARD_GITHUB;
  APP_STATE.saveToFile();
  const uint32_t intervalS = SETTINGS.githubRefreshMinutes * 60u;
  LOG_INF("GH", "Dashboard armed, sleeping for %u s", (unsigned)intervalS);
  enterDashboardSleep(intervalS);  // does not return
}

void GithubDashboardActivity::exitDashboardMode() {
  if (APP_STATE.activeDashboardMode == CrossPointState::DASHBOARD_GITHUB) {
    APP_STATE.activeDashboardMode = CrossPointState::DASHBOARD_NONE;
    APP_STATE.saveToFile();
  }
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

// Small 4x4 contribution-grid glyph for the footer branding.
namespace {
void drawGithubBrandIcon(const GfxRenderer& renderer, int x, int y) {
  static constexpr uint8_t pattern[4] = {0b1011, 0b0110, 0b1101, 0b1011};  // 1 = filled
  constexpr int cell = 5, gap = 2, pitch = cell + gap;
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

void GithubDashboardActivity::renderDashboard() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  const bool portrait = SETTINGS.lockScreenOrientation == CrossPointSettings::LOCK_ORIENT_PORTRAIT;
  const auto origOrientation = renderer.getOrientation();
  renderer.setOrientation(portrait ? GfxRenderer::Orientation::Portrait
                                    : GfxRenderer::Orientation::LandscapeCounterClockwise);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int sideMargin = portrait ? 30 : 40;

  renderer.clearScreen();

  // --- Stat data (shared by both orientations) ---
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

  char hero[16];
  DashboardUI::formatCompact(statTotal, hero, sizeof(hero));

  // Heatmap geometry differs per orientation; the draw loop is shared below.
  int hmCellW, hmPitchX, hmCellH, hmPitchY, hmX0, hmY0;
  const int hmLabelGutter = 34;

  if (portrait) {
    // Hero top, caption, 2x2 grid, divider, then a wide-but-short heatmap.
    renderer.fillRectDither(sideMargin - 16, 42, 8, 74, Color::DarkGray);
    DashboardUI::drawBigText(renderer, sideMargin, 42, hero, 10);
    renderer.drawText(UI_10_FONT_ID, sideMargin, 130, tr(STR_GITHUB_CONTRIB_CAPTION));

    renderer.fillRect(sideMargin, 176, pageWidth - 2 * sideMargin, 1);

    for (int s = 0; s < 4; s++) {
      const int colX = sideMargin + 16 + (s % 2) * ((pageWidth - 2 * sideMargin) / 2 + 8);
      const int rowY = 200 + (s / 2) * 92;
      DashboardUI::drawStatTile(renderer, colX, rowY, stats[s].value, stats[s].label);
    }

    renderer.fillRect(sideMargin, 392, pageWidth - 2 * sideMargin, 1);

    hmCellW = 6;
    hmPitchX = 7;
    hmCellH = 13;
    hmPitchY = 16;
    hmY0 = 448;
  } else {
    // Hero top-left, 2x2 grid top-right, divider, then a full-width heatmap.
    renderer.fillRectDither(sideMargin - 16, 42, 8, 74, Color::DarkGray);
    DashboardUI::drawBigText(renderer, sideMargin, 42, hero, 10);
    renderer.drawText(UI_10_FONT_ID, sideMargin, 130, tr(STR_GITHUB_CONTRIB_CAPTION));

    for (int s = 0; s < 4; s++) {
      const int colX = 430 + (s % 2) * 190;
      const int rowY = 42 + (s / 2) * 78;
      DashboardUI::drawStatTile(renderer, colX, rowY, stats[s].value, stats[s].label);
    }

    renderer.fillRect(sideMargin, 196, pageWidth - 2 * sideMargin, 1);

    hmCellW = 11;
    hmPitchX = 13;
    hmCellH = 14;
    hmPitchY = 17;
    hmY0 = 248;
  }

  // --- Heatmap: weeks as columns, Sunday-first rows, GitHub-style grid ---
  if (!cells.empty()) {
    const int offset = DashboardUI::dayOfWeekSunday0(cells.front().date);
    const int weeks = (offset + static_cast<int>(cells.size()) + 6) / 7;
    const int gridW = weeks * hmPitchX;
    hmX0 = std::max(sideMargin + hmLabelGutter, (pageWidth - gridW + hmLabelGutter) / 2);

    // Weekday labels like GitHub (rows are Sunday-first)
    renderer.drawText(SMALL_FONT_ID, hmX0 - hmLabelGutter, hmY0 + 1 * hmPitchY - 1, "Mon");
    renderer.drawText(SMALL_FONT_ID, hmX0 - hmLabelGutter, hmY0 + 3 * hmPitchY - 1, "Wed");
    renderer.drawText(SMALL_FONT_ID, hmX0 - hmLabelGutter, hmY0 + 5 * hmPitchY - 1, "Fri");

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
        renderer.drawText(SMALL_FONT_ID, hmX0 + col * hmPitchX, hmY0 - 20, MONTH_ABBREV[month - 1]);
        lastLabelCol = col;
      }
    }

    // Crisp level ramp for 1-bit e-ink: dot, outline, gray fill, solid black.
    for (size_t i = 0; i < cells.size(); i++) {
      const int slot = offset + static_cast<int>(i);
      const int x = hmX0 + (slot / 7) * hmPitchX;
      const int y = hmY0 + (slot % 7) * hmPitchY;
      switch (cells[i].level) {
        case 0:
          break;  // blank — dots for every empty day read as noise on e-ink
        case 1:
          renderer.drawRoundedRect(x, y, hmCellW, hmCellH, 1, 2, true);
          break;
        case 2:
          renderer.drawRoundedRect(x, y, hmCellW, hmCellH, 1, 2, true);
          renderer.fillRectDither(x + 2, y + 2, hmCellW - 4, hmCellH - 4, Color::DarkGray);
          break;
        default:
          renderer.fillRoundedRect(x, y, hmCellW, hmCellH, 2, Color::Black);
          break;
      }
    }
  }

  // --- Footer bar: GitHub branding, updated time, username + battery ---
  DashboardUI::drawFooter(renderer, metrics, pageWidth, pageHeight, sideMargin, drawGithubBrandIcon, "GitHub",
                          tr(STR_DASHBOARD_UPDATED), lastUpdated, SETTINGS.githubUsername);

  // Full refresh: this frame stays on the panel for the whole sleep hour.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  renderer.setOrientation(origOrientation);
}
