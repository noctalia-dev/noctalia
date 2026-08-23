#include "render/scene/input_area.h"
#include "render/scene/node.h"

#include <chrono>
#include <cstdio>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <wayland-client-protocol.h>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "input_area_test: {}", message);
      return false;
    }
    return true;
  }

  // One vertical axis frame from a wheel the compositor counted notches for; `lines` is the
  // detent count it reported, which a scroll-factor can inflate past 1.
  void wheelFrame(InputArea& area, float lines, std::uint32_t gestureSerial = 0) {
    const auto value120 = static_cast<std::int32_t>(lines * 120.0f);
    static_cast<void>(area.dispatchAxis(
        1.0f, 1.0f, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, lines * 15.0f, 0, value120, lines,
        gestureSerial
    ));
  }

  // One vertical axis frame from a touchpad: continuous units, no detent count.
  void fingerFrame(InputArea& area, double value) {
    static_cast<void>(
        area.dispatchAxis(1.0f, 1.0f, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_FINGER, value, 0, 0, 0.0f)
    );
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

  {
    InputArea area;
    area.setSize(20.0f, 20.0f);

    int cancellations = 0;
    int clicks = 0;
    area.setOnCancel([&cancellations]() { ++cancellations; });
    area.setOnClick([&clicks](const InputArea::PointerData&) { ++clicks; });

    area.dispatchPress(10.0f, 10.0f, BTN_LEFT, true);
    ok = expect(area.pressed(), "press before cancellation sets pressed state") && ok;

    area.dispatchCancel();

    ok = expect(cancellations == 1, "cancellation callback fires exactly once") && ok;
    ok = expect(!area.pressed(), "cancellation clears pressed state") && ok;

    area.dispatchPress(10.0f, 10.0f, BTN_LEFT, false);
    ok = expect(clicks == 0, "release after cancellation does not click") && ok;
  }

  {
    Node root;
    root.setSize(20.0f, 20.0f);
    auto overflow = std::make_unique<InputArea>();
    auto* overflowPtr = overflow.get();
    overflow->setPosition(25.0f, 0.0f);
    overflow->setSize(10.0f, 10.0f);
    root.addChild(std::move(overflow));

    ok = expect(Node::hitTest(&root, 27.0f, 5.0f) == overflowPtr, "default hit test preserves child overflow") && ok;
    ok = expect(Node::hitTestStrict(&root, 27.0f, 5.0f) == nullptr, "strict hit test clips at ancestor bounds") && ok;
  }

  {
    // The axis counterpart of the button mask: a direction the area has given up is reported
    // unconsumed, so the dispatcher can carry it to an ancestor that claims it.
    InputArea area;
    area.setSize(20.0f, 20.0f);
    int verticalEvents = 0;
    area.setOnAxisHandler([&verticalEvents](const InputArea::PointerData&) {
      ++verticalEvents;
      return true;
    });

    // Wayland reports up as a negative delta.
    area.setAcceptedScrollDirections(
        InputArea::allScrollDirections() & ~InputArea::scrollDirectionMask(InputArea::ScrollDirection::Up)
    );

    const bool upConsumed = area.dispatchAxis(
        1.0f, 1.0f, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, -10.0, 0, 0, -1.0f
    );
    ok = expect(!upConsumed, "an unaccepted scroll direction is reported unconsumed") && ok;
    ok = expect(verticalEvents == 0, "an unaccepted scroll direction never reaches the handler") && ok;

    const bool downConsumed =
        area.dispatchAxis(1.0f, 1.0f, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, 10.0, 0, 0, 1.0f);
    ok = expect(downConsumed, "an accepted scroll direction is still consumed") && ok;
    ok = expect(verticalEvents == 1, "an accepted scroll direction reaches the handler") && ok;

    // Rejection happens before the accumulator, so the given-up direction banks nothing here.
    ok = expect(
             area.dispatchAxis(
                 1.0f, 1.0f, WL_POINTER_AXIS_HORIZONTAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, -10.0, 0, 0, -1.0f
             ),
             "an untouched axis keeps its directions"
         )
        && ok;
  }

  {
    // A notch the compositor counted (value120) steps every time, however fast the wheel turns:
    // discrete steppers scale with how far the user actually scrolled.
    InputArea area;
    area.setSize(20.0f, 20.0f);
    float steps = 0.0f;
    area.setOnAxisHandler([&steps](const InputArea::PointerData& data) {
      steps += data.scrollSteps();
      return true;
    });

    for (int notch = 0; notch < 5; ++notch) {
      wheelFrame(area, 1.0f);
    }
    ok = expect(steps == 5.0f, "back-to-back wheel notches each step") && ok;

    // Flicking back the other way is never swallowed by a gate armed in the old direction.
    steps = 0.0f;
    wheelFrame(area, -1.0f);
    ok = expect(steps == -1.0f, "an immediate reversal steps") && ok;

    // A compositor scroll-factor inflates the delta; the notch the user felt is still one step.
    steps = 0.0f;
    wheelFrame(area, 3.2f);
    ok = expect(steps == 1.0f, "a scaled wheel notch is still one step") && ok;
  }

  {
    // Sub-detent frames (a free-spinning hi-res wheel) accrue to a whole notch before stepping.
    InputArea area;
    area.setSize(20.0f, 20.0f);
    float steps = 0.0f;
    area.setOnAxisHandler([&steps](const InputArea::PointerData& data) {
      steps += data.scrollSteps();
      return true;
    });

    for (int frame = 0; frame < 3; ++frame) {
      wheelFrame(area, 0.25f);
    }
    ok = expect(steps == 0.0f, "sub-detent frames do not step on their own") && ok;

    wheelFrame(area, 0.25f);
    ok = expect(steps == 1.0f, "sub-detent frames step once a full detent has turned") && ok;
  }

  {
    // A touchpad carries no detent count and streams frames as fast as the finger moves, so
    // steps are rate-capped rather than fired on every threshold crossing.
    InputArea area;
    area.setSize(20.0f, 20.0f);
    float steps = 0.0f;
    area.setOnAxisHandler([&steps](const InputArea::PointerData& data) {
      steps += data.scrollSteps();
      return true;
    });

    for (int frame = 0; frame < 20; ++frame) {
      fingerFrame(area, 25.0);
    }
    ok = expect(steps == 1.0f, "a touchpad flick is rate-capped to one step per interval") && ok;

    // The cap is a rate, not a one-shot: keep swiping and it keeps stepping, no pause needed.
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    fingerFrame(area, 25.0);
    ok = expect(steps == 2.0f, "a touchpad keeps stepping once the cap interval elapses") && ok;
  }

  {
    // scrollStepStartsGesture() is what a list-cycling consumer acts on: it marks the first step
    // of a flick, so the rest of the burst can be swallowed and one flick moves one position.
    InputArea area;
    area.setSize(20.0f, 20.0f);
    float steps = 0.0f;
    area.setOnAxisHandler([&steps](const InputArea::PointerData& data) {
      if (data.scrollStepStartsGesture()) {
        steps += data.scrollSteps();
      }
      return true;
    });

    for (int notch = 0; notch < 5; ++notch) {
      wheelFrame(area, 1.0f);
    }
    ok = expect(steps == 1.0f, "only the first step of a flick starts the gesture") && ok;

    // Reversing mid-flick is a new intent, not a continuation of the same one.
    steps = 0.0f;
    wheelFrame(area, -1.0f);
    ok = expect(steps == -1.0f, "reversing within a burst starts a new gesture") && ok;

    steps = 0.0f;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    wheelFrame(area, -1.0f);
    ok = expect(steps == -1.0f, "a quiet stream starts the next gesture") && ok;
  }

  {
    // axis_stop advances the stream serial, so another same-direction gesture starts immediately
    // without waiting for the silence timeout.
    InputArea area;
    area.setSize(20.0f, 20.0f);
    int gestureStarts = 0;
    area.setOnAxisHandler([&gestureStarts](const InputArea::PointerData& data) {
      if (data.scrollStepStartsGesture()) {
        ++gestureStarts;
      }
      return true;
    });

    wheelFrame(area, 1.0f, 7);
    wheelFrame(area, 1.0f, 7);
    ok = expect(gestureStarts == 1, "one axis stream starts one gesture") && ok;

    wheelFrame(area, 1.0f, 8);
    ok = expect(gestureStarts == 2, "axis stop serial starts the next gesture immediately") && ok;
  }

  {
    InputArea area;
    std::string title = "old";
    int refreshes = 0;
    area.setTooltipProvider([&title]() -> TooltipContent { return title; });
    area.setTooltipChangedCallback([&refreshes](InputArea*) { ++refreshes; });

    title = "new";
    area.requestTooltipRefresh();
    ok = expect(refreshes == 0, "tooltip refresh stays dormant while not hovered") && ok;

    area.dispatchEnter(0.0f, 0.0f);
    area.requestTooltipRefresh();
    ok = expect(refreshes == 1, "tooltip refresh notifies while hovered") && ok;
    ok = expect(
             std::get<std::string>(area.tooltipContent()) == "new", "event refresh reads the provider's latest content"
         )
        && ok;

    area.dispatchLeave();
    area.requestTooltipRefresh();
    ok = expect(refreshes == 1, "tooltip refresh stops notifying after hover leaves") && ok;
  }

  return ok ? 0 : 1;
}
