#include "render/scene/input_area.h"

#include <cstdio>
#include <linux/input-event-codes.h>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "input_area_test: %s\n", message);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  bool ok = true;

  {
    InputArea area;
    area.setSize(20.0f, 20.0f);

    int clicks = 0;
    area.setOnClick([&clicks](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) {
        ++clicks;
      }
    });

    area.dispatchPress(10.0f, 10.0f, BTN_LEFT, true);
    area.dispatchPress(12.0f, 12.0f, BTN_LEFT, false);

    ok = expect(clicks == 1, "release inside fires click") && ok;
    ok = expect(!area.pressed(), "inside release clears pressed state") && ok;
  }

  {
    InputArea area;
    area.setSize(20.0f, 20.0f);

    int clicks = 0;
    int releases = 0;
    area.setOnClick([&clicks](const InputArea::PointerData&) { ++clicks; });
    area.setOnPress([&releases](const InputArea::PointerData& data) {
      if (!data.pressed) {
        ++releases;
      }
    });

    area.dispatchPress(10.0f, 10.0f, BTN_LEFT, true);
    ok = expect(area.pressed(), "press sets pressed state") && ok;

    area.dispatchPress(30.0f, 10.0f, BTN_LEFT, false);

    ok = expect(clicks == 0, "release outside cancels click") && ok;
    ok = expect(releases == 1, "outside release still dispatches press release") && ok;
    ok = expect(!area.pressed(), "outside release clears pressed state") && ok;
  }

  {
    InputArea area;
    area.setSize(20.0f, 20.0f);
    area.setHitTestOutset({.right = 10.0f});

    int clicks = 0;
    area.setOnClick([&clicks](const InputArea::PointerData&) { ++clicks; });

    area.dispatchPress(10.0f, 10.0f, BTN_LEFT, true);
    area.dispatchPress(25.0f, 10.0f, BTN_LEFT, false);

    ok = expect(clicks == 1, "release inside hit-test outset fires click") && ok;
  }

  return ok ? 0 : 1;
}
