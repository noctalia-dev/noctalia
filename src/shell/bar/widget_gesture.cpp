#include "shell/bar/widget_gesture.h"

#include <algorithm>
#include <array>
#include <linux/input-event-codes.h>
#include <wayland-client-protocol.h>

namespace noctalia::bar {

  namespace {

    // Mice report the thumb buttons as either SIDE/EXTRA or BACK/FORWARD, so both pairs bind.
    constexpr std::array<std::uint32_t, 1> kLeftButtons{BTN_LEFT};
    constexpr std::array<std::uint32_t, 1> kRightButtons{BTN_RIGHT};
    constexpr std::array<std::uint32_t, 1> kMiddleButtons{BTN_MIDDLE};
    constexpr std::array<std::uint32_t, 2> kBackButtons{BTN_SIDE, BTN_BACK};
    constexpr std::array<std::uint32_t, 2> kForwardButtons{BTN_EXTRA, BTN_FORWARD};

    struct GestureEntry {
      Gesture gesture;
      std::string_view configKey;
      std::string_view labelKey;
      std::span<const std::uint32_t> buttons;
    };

    constexpr std::array<GestureEntry, kGestureCount> kGestures{{
        {Gesture::Left, "left", "settings.widgets.gestures.left", kLeftButtons},
        {Gesture::Right, "right", "settings.widgets.gestures.right", kRightButtons},
        {Gesture::Middle, "middle", "settings.widgets.gestures.middle", kMiddleButtons},
        {Gesture::Back, "back", "settings.widgets.gestures.back", kBackButtons},
        {Gesture::Forward, "forward", "settings.widgets.gestures.forward", kForwardButtons},
        {Gesture::ScrollUp, "scroll_up", "settings.widgets.gestures.scroll-up", {}},
        {Gesture::ScrollDown, "scroll_down", "settings.widgets.gestures.scroll-down", {}},
        {Gesture::ScrollLeft, "scroll_left", "settings.widgets.gestures.scroll-left", {}},
        {Gesture::ScrollRight, "scroll_right", "settings.widgets.gestures.scroll-right", {}},
    }};

    constexpr std::array<Gesture, kGestureCount> kGestureOrder{
        Gesture::Left,     Gesture::Right,      Gesture::Middle,     Gesture::Back,        Gesture::Forward,
        Gesture::ScrollUp, Gesture::ScrollDown, Gesture::ScrollLeft, Gesture::ScrollRight,
    };

    [[nodiscard]] const GestureEntry& entryFor(Gesture gesture) noexcept {
      return kGestures[static_cast<std::size_t>(gesture)];
    }

  } // namespace

  std::span<const Gesture> allGestures() noexcept { return kGestureOrder; }

  std::string_view gestureConfigKey(Gesture gesture) noexcept { return entryFor(gesture).configKey; }

  std::string_view gestureLabelKey(Gesture gesture) noexcept { return entryFor(gesture).labelKey; }

  std::optional<Gesture> parseGestureKey(std::string_view key) noexcept {
    const auto it = std::ranges::find(kGestures, key, &GestureEntry::configKey);
    if (it == kGestures.end()) {
      return std::nullopt;
    }
    return it->gesture;
  }

  std::optional<ScrollRepeatMode> parseScrollRepeatMode(std::string_view value) noexcept {
    if (value == "auto") {
      return ScrollRepeatMode::Auto;
    }
    if (value == "gesture") {
      return ScrollRepeatMode::Gesture;
    }
    if (value == "steps") {
      return ScrollRepeatMode::Steps;
    }
    return std::nullopt;
  }

  bool scrollRepeatsEveryStep(ScrollRepeatMode mode, bool actionCycles) noexcept {
    switch (mode) {
    case ScrollRepeatMode::Auto:
      return !actionCycles;
    case ScrollRepeatMode::Gesture:
      return false;
    case ScrollRepeatMode::Steps:
      return true;
    }
    return false;
  }

  std::optional<Gesture> gestureForButton(std::uint32_t button) noexcept {
    switch (button) {
    case BTN_LEFT:
      return Gesture::Left;
    case BTN_RIGHT:
      return Gesture::Right;
    case BTN_MIDDLE:
      return Gesture::Middle;
    case BTN_SIDE:
    case BTN_BACK:
      return Gesture::Back;
    case BTN_EXTRA:
    case BTN_FORWARD:
      return Gesture::Forward;
    default:
      return std::nullopt;
    }
  }

  std::optional<Gesture> gestureForScroll(std::uint32_t axis, float steps) noexcept {
    if (steps == 0.0f) {
      return std::nullopt;
    }
    // Wayland reports up/left as a negative delta.
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return steps < 0.0f ? Gesture::ScrollUp : Gesture::ScrollDown;
    }
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      return steps < 0.0f ? Gesture::ScrollLeft : Gesture::ScrollRight;
    }
    return std::nullopt;
  }

  std::span<const std::uint32_t> buttonsForGesture(Gesture gesture) noexcept { return entryFor(gesture).buttons; }

  std::optional<InputArea::ScrollDirection> scrollDirectionForGesture(Gesture gesture) noexcept {
    switch (gesture) {
    case Gesture::ScrollUp:
      return InputArea::ScrollDirection::Up;
    case Gesture::ScrollDown:
      return InputArea::ScrollDirection::Down;
    case Gesture::ScrollLeft:
      return InputArea::ScrollDirection::Left;
    case Gesture::ScrollRight:
      return InputArea::ScrollDirection::Right;
    default:
      return std::nullopt;
    }
  }

} // namespace noctalia::bar
