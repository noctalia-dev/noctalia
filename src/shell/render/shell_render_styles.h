#pragma once

#include "render/core/color.h"

#include <cstdint>

struct CountdownRingStyle {
  Color color{};
  float thickness = 6.0F;
  float progress = 1.0F;
};

enum class ScreenCornerPosition : std::uint8_t {
  TopLeft,
  TopRight,
  BottomRight,
  BottomLeft,
};

struct ScreenCornerStyle {
  Color color = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  ScreenCornerPosition position = ScreenCornerPosition::TopLeft;
  float exponent = 4.0F;
};

constexpr bool operator==(const ScreenCornerStyle& lhs, const ScreenCornerStyle& rhs) noexcept {
  return lhs.color == rhs.color && lhs.position == rhs.position && lhs.exponent == rhs.exponent;
}
