#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// "Lock Screens" folder: a simple menu of the always-on timed-poll dashboards
// (GitHub / Weather / Tempest, with room for more later) so Home's top level
// doesn't grow one entry per dashboard as new ones get added.
class LockScreensActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  static constexpr int ITEM_COUNT = 3;

  void onGithubOpen();
  void onWeatherOpen();
  void onTempestOpen();

 public:
  explicit LockScreensActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LockScreens", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
