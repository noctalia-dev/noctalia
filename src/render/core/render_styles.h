#pragma once

#include "render/core/color.h"

#include <array>
#include <cstdint>

enum class FillMode {
  None,
  Solid,
  LinearGradient,
};

enum class GradientDirection {
  Horizontal,
  Vertical,
};

enum class CornerShape {
  Convex,
  Concave,
};

struct CornerShapes {
  CornerShape tl = CornerShape::Convex;
  CornerShape tr = CornerShape::Convex;
  CornerShape br = CornerShape::Convex;
  CornerShape bl = CornerShape::Convex;
};

constexpr bool operator==(const CornerShapes& lhs, const CornerShapes& rhs) noexcept {
  return lhs.tl == rhs.tl && lhs.tr == rhs.tr && lhs.br == rhs.br && lhs.bl == rhs.bl;
}

struct RectInsets {
  float left = 0.0F;
  float top = 0.0F;
  float right = 0.0F;
  float bottom = 0.0F;
};

constexpr bool operator==(const RectInsets& lhs, const RectInsets& rhs) noexcept {
  return lhs.left == rhs.left && lhs.top == rhs.top && lhs.right == rhs.right && lhs.bottom == rhs.bottom;
}

// Per-corner radii: top-left, top-right, bottom-right, bottom-left.
// Implicit construction from a single float sets all four corners uniformly,
// so existing `.radius = value` assignments continue to compile unchanged.
struct Radii {
  float tl = 0.0F;
  float tr = 0.0F;
  float br = 0.0F;
  float bl = 0.0F;

  Radii() = default;
  /* implicit */ Radii(float r) : tl(r), tr(r), br(r), bl(r) {} // NOLINT(google-explicit-constructor)
  Radii(float tlv, float trv, float brv, float blv) : tl(tlv), tr(trv), br(brv), bl(blv) {}
};

constexpr bool operator==(const Radii& lhs, const Radii& rhs) noexcept {
  return lhs.tl == rhs.tl && lhs.tr == rhs.tr && lhs.br == rhs.br && lhs.bl == rhs.bl;
}

struct GradientStop {
  float position = 0.0F;
  Color color{};
};

constexpr bool operator==(const GradientStop& lhs, const GradientStop& rhs) noexcept {
  return lhs.position == rhs.position && lhs.color == rhs.color;
}

// Linear-gradient darkening applied to an image's texels inside the image draw, so the image and
// its scrim resolve to a single antialiased edge instead of two stacked rounded rects. Only the
// color is affected; transparent texels stay transparent.
struct ImageScrim {
  GradientDirection direction = GradientDirection::Horizontal;
  std::array<GradientStop, 4> stops{};
  bool enabled = false;
};

constexpr bool operator==(const ImageScrim& lhs, const ImageScrim& rhs) noexcept {
  return lhs.enabled == rhs.enabled && lhs.direction == rhs.direction && lhs.stops == rhs.stops;
}

struct RoundedRectStyle {
  Color fill{};
  Color border{};
  FillMode fillMode = FillMode::Solid;
  GradientDirection gradientDirection = GradientDirection::Horizontal;
  std::array<GradientStop, 4> gradientStops{};
  CornerShapes corners{};
  RectInsets logicalInset{};
  Radii radius;
  float softness = 1.0F;
  bool noAa = false;
  bool invertFill = false;
  float borderWidth = 0.0F;
  bool outerShadow = false;
  float shadowCutoutOffsetX = 0.0F;
  float shadowCutoutOffsetY = 0.0F;
  bool shadowExclusion = false;
  float shadowExclusionOffsetX = 0.0F;
  float shadowExclusionOffsetY = 0.0F;
  float shadowExclusionWidth = 0.0F;
  float shadowExclusionHeight = 0.0F;
  CornerShapes shadowExclusionCorners{};
  RectInsets shadowExclusionLogicalInset{};
  Radii shadowExclusionRadius;
};

constexpr bool operator==(const RoundedRectStyle& lhs, const RoundedRectStyle& rhs) noexcept {
  return lhs.fill == rhs.fill
      && lhs.border == rhs.border
      && lhs.fillMode == rhs.fillMode
      && lhs.gradientDirection == rhs.gradientDirection
      && lhs.corners == rhs.corners
      && lhs.gradientStops == rhs.gradientStops
      && lhs.logicalInset == rhs.logicalInset
      && lhs.radius == rhs.radius
      && lhs.softness == rhs.softness
      && lhs.noAa == rhs.noAa
      && lhs.invertFill == rhs.invertFill
      && lhs.borderWidth == rhs.borderWidth
      && lhs.outerShadow == rhs.outerShadow
      && lhs.shadowCutoutOffsetX == rhs.shadowCutoutOffsetX
      && lhs.shadowCutoutOffsetY == rhs.shadowCutoutOffsetY
      && lhs.shadowExclusion == rhs.shadowExclusion
      && lhs.shadowExclusionOffsetX == rhs.shadowExclusionOffsetX
      && lhs.shadowExclusionOffsetY == rhs.shadowExclusionOffsetY
      && lhs.shadowExclusionWidth == rhs.shadowExclusionWidth
      && lhs.shadowExclusionHeight == rhs.shadowExclusionHeight
      && lhs.shadowExclusionCorners == rhs.shadowExclusionCorners
      && lhs.shadowExclusionLogicalInset == rhs.shadowExclusionLogicalInset
      && lhs.shadowExclusionRadius == rhs.shadowExclusionRadius;
}

struct SpinnerStyle {
  Color color{};
  float thickness = 2.0F;
};
