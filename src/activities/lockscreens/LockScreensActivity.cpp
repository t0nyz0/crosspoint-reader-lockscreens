#include "LockScreensActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void LockScreensActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void LockScreensActivity::loop() {
  buttonNavigator.onNext([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, ITEM_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (selectorIndex) {
      case 0:
        onGithubOpen();
        break;
      case 1:
        onWeatherOpen();
        break;
      case 2:
        onTempestOpen();
        break;
      default:
        break;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void LockScreensActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LOCK_SCREENS));

  const std::vector<const char*> items = {tr(STR_GITHUB_DASHBOARD), tr(STR_WEATHER_DASHBOARD),
                                          tr(STR_TEMPEST_DASHBOARD)};
  const std::vector<UIIcon> icons = {Github, Weather, Tempest};

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.buttonHintsHeight)},
      static_cast<int>(items.size()), selectorIndex, [&items](int index) { return std::string(items[index]); },
      [&icons](int index) { return icons[index]; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void LockScreensActivity::onGithubOpen() { activityManager.goToGithubDashboard(); }

void LockScreensActivity::onWeatherOpen() { activityManager.goToWeatherDashboard(); }

void LockScreensActivity::onTempestOpen() { activityManager.goToTempestDashboard(); }
