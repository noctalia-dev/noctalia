#pragma once

#include "render/core/render_styles.h"

#include <cstdint>

enum class ShadowDirection : std::uint8_t {
  Center = 0,
  Down = 1,
  Up = 2,
  Left = 3,
  Right = 4,
  DownLeft = 5,
  DownRight = 6,
  UpLeft = 7,
  UpRight = 8,
};

struct ShadowDirectionOffset {
  std::int32_t x;
  std::int32_t y;
};

constexpr ShadowDirectionOffset shadowDirectionOffset(ShadowDirection dir) noexcept {
  switch (dir) {
  case ShadowDirection::Center:
    return {0, 0};
  case ShadowDirection::Down:
    return {0, 2};
  case ShadowDirection::Up:
    return {0, -2};
  case ShadowDirection::Left:
    return {-2, 0};
  case ShadowDirection::Right:
    return {2, 0};
  case ShadowDirection::DownLeft:
    return {-2, 2};
  case ShadowDirection::DownRight:
    return {2, 2};
  case ShadowDirection::UpLeft:
    return {-2, -2};
  case ShadowDirection::UpRight:
    return {2, -2};
  }
  return {0, 2};
}

struct ShadowStyle {
  ShadowDirection direction = ShadowDirection::Down;
  float alpha = 0.55F;

  bool operator==(const ShadowStyle&) const = default;
};

namespace surface_shadow {

  constexpr std::int32_t kBlurRadius = 12;
  constexpr int kMaskPadding = 6;
  constexpr float kMaskSigma = 2.0F;

  struct MaskStyle {
    Color tint;
    ShadowDirectionOffset offset;
  };

  [[nodiscard]] MaskStyle maskStyle(const ShadowStyle& shadow) noexcept;

  struct Bleed {
    std::int32_t left = 0;
    std::int32_t right = 0;
    std::int32_t up = 0;
    std::int32_t down = 0;
  };

  struct Shape {
    CornerShapes corners{};
    RectInsets logicalInset{};
    Radii radius;
  };

  bool enabled(bool componentShadow, const ShadowStyle& shadow) noexcept;
  Bleed bleed(bool componentShadow, const ShadowStyle& shadow) noexcept;
  RoundedRectStyle style(const ShadowStyle& shadow, float backgroundOpacity, const Shape& shape) noexcept;
  bool sameSurfaceMetrics(const ShadowStyle& previous, const ShadowStyle& next) noexcept;

} // namespace surface_shadow
