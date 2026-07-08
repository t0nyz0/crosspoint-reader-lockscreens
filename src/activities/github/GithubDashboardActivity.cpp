#include "GithubDashboardActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
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

void GithubDashboardActivity::runFetch() {
  cells.clear();
  cells.reserve(MAX_CELLS);
  totalContributions.clear();
  parseBuf.clear();

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
    LOG_INF("GH", "Parsed %u days, total \"%s\"", (unsigned)cells.size(), totalContributions.c_str());
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
  while (cells.size() < MAX_CELLS) {
    const size_t td = parseBuf.find("<td", consumed);
    if (td == std::string::npos) {
      consumed = parseBuf.size() > PARSE_TAIL_KEEP ? parseBuf.size() - PARSE_TAIL_KEEP : 0;
      break;
    }
    const size_t end = parseBuf.find('>', td);
    if (end == std::string::npos) {
      consumed = td;  // incomplete tag, wait for the next chunk
      break;
    }

    size_t dStart = 0, dLen = 0, lStart = 0, lLen = 0;
    if (findAttr(parseBuf, td, end, "data-date=", dStart, dLen) && dLen == 10 &&
        findAttr(parseBuf, td, end, "data-level=", lStart, lLen) && lLen >= 1) {
      ContribCell cell;
      memcpy(cell.date, parseBuf.data() + dStart, 10);
      cell.date[10] = '\0';
      const char lvl = parseBuf[lStart];
      cell.level = (lvl >= '0' && lvl <= '4') ? lvl - '0' : 0;
      cells.push_back(cell);
    }
    consumed = end + 1;
  }

  if (totalContributions.empty()) {
    scanForTotal();
  }
  if (consumed > 0) {
    parseBuf.erase(0, consumed);
  }
  return true;
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

void GithubDashboardActivity::renderDashboard() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Header
  char title[64];
  snprintf(title, sizeof(title), "@%s", SETTINGS.githubUsername);
  renderer.drawCenteredText(UI_12_FONT_ID, 290, title, true, EpdFontFamily::BOLD);

  if (!totalContributions.empty()) {
    char line[96];
    snprintf(line, sizeof(line), "%s %s", totalContributions.c_str(), tr(STR_GITHUB_CONTRIB_SUFFIX));
    renderer.drawCenteredText(UI_10_FONT_ID, 330, line);
  }

  // Contribution heatmap: weeks as columns, Sunday-first rows, like GitHub.
  if (!cells.empty()) {
    const int offset = dayOfWeekSunday0(cells.front().date);
    const int weeks = (offset + static_cast<int>(cells.size()) + 6) / 7;
    constexpr int pitch = 8;    // 7 px cell + 1 px gap
    constexpr int cellSz = 7;
    const int gridW = weeks * pitch;
    const int x0 = std::max(0, (pageWidth - gridW) / 2);
    const int y0 = 400;

    // Month labels above the columns where a new month starts on a week boundary
    int lastLabelCol = -100;
    for (size_t i = 0; i < cells.size(); i++) {
      const int slot = offset + static_cast<int>(i);
      if (slot % 7 != 0) continue;  // only the top (Sunday) row starts a column
      const int month = atoi(cells[i].date + 5);
      const int day = atoi(cells[i].date + 8);
      if (day > 7) continue;  // not the first week of the month
      const int col = slot / 7;
      if (col - lastLabelCol < 4) continue;  // avoid overlapping labels
      if (month >= 1 && month <= 12) {
        renderer.drawText(SMALL_FONT_ID, x0 + col * pitch, y0 - 18, MONTH_ABBREV[month - 1]);
        lastLabelCol = col;
      }
    }

    for (size_t i = 0; i < cells.size(); i++) {
      const int slot = offset + static_cast<int>(i);
      const int x = x0 + (slot / 7) * pitch;
      const int y = y0 + (slot % 7) * pitch;
      switch (cells[i].level) {
        case 0:
          // faint presence so the grid reads as a calendar, like GitHub's empty cells
          renderer.fillRect(x + 3, y + 3, 1, 1);
          break;
        case 1:
          renderer.fillRectDither(x, y, cellSz, cellSz, Color::LightGray);
          break;
        case 2:
          renderer.fillRectDither(x, y, cellSz, cellSz, Color::DarkGray);
          break;
        default:
          renderer.fillRect(x, y, cellSz, cellSz);
          break;
      }
    }
  }

  // Footer: optional last-updated time and a minimal battery icon
  int footerY = pageHeight - 60;
  if (SETTINGS.clockHasBeenSynced) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      char line[32];
      snprintf(line, sizeof(line), "%s %s", tr(STR_GITHUB_UPDATED), timeBuf);
      renderer.drawCenteredText(SMALL_FONT_ID, footerY, line);
    }
  }
  GUI.drawBatteryLeft(
      renderer,
      Rect{(pageWidth - metrics.batteryWidth) / 2, footerY + 25, metrics.batteryWidth, metrics.batteryHeight},
      false);

  // Full refresh: this frame stays on the panel for the whole sleep hour.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
