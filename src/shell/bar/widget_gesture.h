#pragma once

#include "render/scene/input_area.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace noctalia::bar {

  // Pointer gestures a bar widget can bind an action to. The bar surface never holds keyboard
  // focus (layer surfaces are created with LayerShellKeyboard::None), so modifier chords such as
  // "ctrl+left" cannot be detected and are not part of the vocabulary.
  enum class Gesture : std::uint8_t {
    Left,
    Right,
    Middle,
    Back,
    Forward,
    ScrollUp,
    ScrollDown,
    ScrollLeft,
    ScrollRight,
  };

  enum class ScrollRepeatMode : std::uint8_t {
    Auto,
    Gesture,
    Steps,
  };

  inline constexpr std::size_t kGestureCount = 9;

  class GestureMask {
  public:
    constexpr GestureMask() = default;
    constexpr GestureMask(std::initializer_list<Gesture> gestures) {
      for (const auto gesture : gestures) {
        set(gesture);
      }
    }

    constexpr void set(Gesture gesture) noexcept { m_bits |= bit(gesture); }
    [[nodiscard]] constexpr bool contains(Gesture gesture) const noexcept { return (m_bits & bit(gesture)) != 0; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_bits == 0; }

  private:
    [[nodiscard]] static constexpr std::uint16_t bit(Gesture gesture) noexcept {
      return static_cast<std::uint16_t>(1U << static_cast<std::uint8_t>(gesture));
    }

    std::uint16_t m_bits = 0;
  };

  [[nodiscard]] std::span<const Gesture> allGestures() noexcept;

  // Canonical config key, e.g. "scroll_up". Keys are the only spelling accepted in config.
  [[nodiscard]] std::string_view gestureConfigKey(Gesture gesture) noexcept;
  [[nodiscard]] std::string_view gestureLabelKey(Gesture gesture) noexcept;
  [[nodiscard]] std::optional<Gesture> parseGestureKey(std::string_view key) noexcept;
  [[nodiscard]] std::optional<ScrollRepeatMode> parseScrollRepeatMode(std::string_view value) noexcept;
  [[nodiscard]] bool scrollRepeatsEveryStep(ScrollRepeatMode mode, bool actionCycles) noexcept;

  // Thumb buttons: mice report them as either BTN_SIDE/BTN_EXTRA or BTN_BACK/BTN_FORWARD, so both
  // pairs resolve to Back/Forward. Returns nullopt for buttons with no gesture.
  [[nodiscard]] std::optional<Gesture> gestureForButton(std::uint32_t button) noexcept;

  // `steps` is InputArea::PointerData::scrollSteps(): whole detents, positive = down/right.
  // Returns nullopt when the axis is unknown or no whole detent accumulated.
  [[nodiscard]] std::optional<Gesture> gestureForScroll(std::uint32_t axis, float steps) noexcept;

  // Mouse buttons that trigger `gesture`, for building an InputArea button mask. Back/Forward
  // yield two codes each; scroll gestures yield none, being delivered on the axis path instead.
  [[nodiscard]] std::span<const std::uint32_t> buttonsForGesture(Gesture gesture) noexcept;
  // The scroll direction a gesture claims, or nullopt for the button gestures.
  [[nodiscard]] std::optional<InputArea::ScrollDirection> scrollDirectionForGesture(Gesture gesture) noexcept;

} // namespace noctalia::bar
