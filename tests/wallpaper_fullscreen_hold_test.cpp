#include "shell/wallpaper/wallpaper_fullscreen_hold.h"
#include "tests/test_check.h"

#include <cstdint>
#include <string>
#include <vector>

int main() {
  {
    wallpaper::FullscreenHold hold;
    TEST_CHECK(!hold.isActive());
    // A repeated request for the same state is not a change.
    TEST_CHECK(!hold.setFullscreenActive(false));
    TEST_CHECK(!hold.isActive());

    TEST_CHECK(hold.setFullscreenActive(true));
    TEST_CHECK(hold.isActive());
    TEST_CHECK(!hold.setFullscreenActive(true));
    TEST_CHECK(hold.isActive());

    TEST_CHECK(hold.setFullscreenActive(false));
    TEST_CHECK(!hold.isActive());
    TEST_CHECK(!hold.setFullscreenActive(false));
  }

  {
    // Without a hold the change is applied straight away and never queued.
    wallpaper::FullscreenHold hold;
    TEST_CHECK(!hold.deferOrApply("/wallpapers/a.jpg", "DP-1"));
    TEST_CHECK(hold.release().empty());
  }

  {
    wallpaper::FullscreenHold hold;
    hold.setFullscreenActive(true);

    TEST_CHECK(hold.deferOrApply("/wallpapers/a.jpg", "DP-1"));
    TEST_CHECK(hold.deferOrApply("/wallpapers/b.jpg", "DP-1"));
    TEST_CHECK(hold.deferOrApply("/wallpapers/c.jpg", ""));

    // The same path and connector as the newest entry collapses instead of piling up.
    TEST_CHECK(hold.deferOrApply("/wallpapers/c.jpg", ""));

    const auto released = hold.release();
    TEST_CHECK(released.size() == 3);
    TEST_CHECK(released[0].path == "/wallpapers/a.jpg");
    TEST_CHECK(released[0].connector == "DP-1");
    TEST_CHECK(released[1].path == "/wallpapers/b.jpg");
    TEST_CHECK(released[1].connector == "DP-1");
    TEST_CHECK(released[2].path == "/wallpapers/c.jpg");
    TEST_CHECK(released[2].connector.empty());

    // The queue is cleared after a release.
    TEST_CHECK(hold.release().empty());
  }

  {
    // Releasing without a hold ever having been active is harmless.
    wallpaper::FullscreenHold hold;
    TEST_CHECK(hold.release().empty());
  }

  return 0;
}
