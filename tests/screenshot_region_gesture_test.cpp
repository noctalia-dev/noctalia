#include "capture/screenshot_region_gesture.h"
#include "tests/test_check.h"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  capture::RegionPress pressAt(double x, double y) {
    return capture::RegionPress{
        .confirming = true,
        .modifierMode = true,
        .modifierHeld = false,
        .rightButton = false,
        .x = x,
        .y = y,
        .x0 = 300.0,
        .y0 = 300.0,
        .x1 = 700.0,
        .y1 = 600.0,
    };
  }

} // namespace

int main() {
  using capture::DragMode;

  // With a modifier configured, a plain drag draws a new selection from anywhere:
  // inside the pending region, on its border, and outside it.
  TEST_CHECK(capture::regionPressGesture(pressAt(500.0, 450.0)) == DragMode::NewSelection);
  TEST_CHECK(capture::regionPressGesture(pressAt(300.0, 450.0)) == DragMode::NewSelection);
  TEST_CHECK(capture::regionPressGesture(pressAt(1000.0, 900.0)) == DragMode::NewSelection);

  // Holding the modifier moves the pending region from anywhere, even outside it.
  {
    auto press = pressAt(500.0, 450.0);
    press.modifierHeld = true;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::Move);
    press.x = 1000.0;
    press.y = 900.0;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::Move);
  }

  // Modifier + right drag resizes from the outer thirds; the center third is ignored.
  {
    auto press = pressAt(310.0, 310.0);
    press.modifierHeld = true;
    press.rightButton = true;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::TopLeftCorner);
    press.x = 690.0;
    press.y = 450.0;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::RightEdge);
    press.x = 500.0;
    press.y = 590.0;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::BottomEdge);
    press.y = 450.0;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::None);
  }

  // A right press without the modifier, or with nothing to adjust, does nothing.
  {
    auto press = pressAt(500.0, 450.0);
    press.rightButton = true;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::None);
    press.modifierHeld = true;
    press.confirming = false;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::None);
    press.rightButton = false;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::NewSelection);
  }

  // Without a modifier configured, a plain drag adjusts the pending region and only a
  // press outside it starts over.
  {
    auto press = pressAt(500.0, 450.0);
    press.modifierMode = false;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::Move);
    press.x = 300.0;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::LeftEdge);
    press.y = 300.0;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::TopLeftCorner);
    press.x = 1000.0;
    press.y = 900.0;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::NewSelection);
    press.confirming = false;
    press.x = 500.0;
    press.y = 450.0;
    TEST_CHECK(capture::regionPressGesture(press) == DragMode::NewSelection);
  }

  // Enter always confirms; Space confirms only while it is not the modifier.
  TEST_CHECK(capture::isRegionConfirmKey(XKB_KEY_Return, false));
  TEST_CHECK(capture::isRegionConfirmKey(XKB_KEY_KP_Enter, false));
  TEST_CHECK(capture::isRegionConfirmKey(XKB_KEY_space, false));
  TEST_CHECK(capture::isRegionConfirmKey(XKB_KEY_Return, true));
  TEST_CHECK(capture::isRegionConfirmKey(XKB_KEY_KP_Enter, true));
  TEST_CHECK(!capture::isRegionConfirmKey(XKB_KEY_space, true));
  TEST_CHECK(!capture::isRegionConfirmKey(XKB_KEY_a, false));

  return 0;
}
