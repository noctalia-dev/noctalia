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
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

constexpr bool operator==(const RectInsets& lhs, const RectInsets& rhs) noexcept {
  return lhs.left == rhs.left && lhs.top == rhs.top && lhs.right == rhs.right && lhs.bottom == rhs.bottom;
}

// Per-corner radii: top-left, top-right, bottom-right, bottom-left.
// Implicit construction from a single float sets all four corners uniformly,
// so existing `.radius = value` assignments continue to compile unchanged.
struct Radii {
  float tl = 0.0f;
  float tr = 0.0f;
  float br = 0.0f;
  float bl = 0.0f;

  Radii() = default;
  /* implicit */ Radii(float r) : tl(r), tr(r), br(r), bl(r) {} // NOLINT(google-explicit-constructor)
  Radii(float tlv, float trv, float brv, float blv) : tl(tlv), tr(trv), br(brv), bl(blv) {}
};

constexpr bool operator==(const Radii& lhs, const Radii& rhs) noexcept {
  return lhs.tl == rhs.tl && lhs.tr == rhs.tr && lhs.br == rhs.br && lhs.bl == rhs.bl;
}

struct GradientStop {
  float position = 0.0f;
  Color color{};
};

constexpr bool operator==(const GradientStop& lhs, const GradientStop& rhs) noexcept {
  return lhs.position == rhs.position && lhs.color == rhs.color;
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
  float softness = 1.0f;
  bool noAa = false;
  bool invertFill = false;
  float borderWidth = 0.0f;
  bool outerShadow = false;
  float shadowCutoutOffsetX = 0.0f;
  float shadowCutoutOffsetY = 0.0f;
  bool shadowExclusion = false;
  float shadowExclusionOffsetX = 0.0f;
  float shadowExclusionOffsetY = 0.0f;
  float shadowExclusionWidth = 0.0f;
  float shadowExclusionHeight = 0.0f;
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
  float thickness = 2.0f;
};

struct CountdownRingStyle {
  Color color{};
  float thickness = 6.0f;
  float progress = 1.0f;
};

enum class ScreenCornerPosition : std::uint8_t {
  TopLeft,
  TopRight,
  BottomRight,
  BottomLeft,
};

struct ScreenCornerStyle {
  Color color = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  ScreenCornerPosition position = ScreenCornerPosition::TopLeft;
  float exponent = 4.0f;
  float softness = 1.0f;
};

constexpr bool operator==(const ScreenCornerStyle& lhs, const ScreenCornerStyle& rhs) noexcept {
  return lhs.color == rhs.color
      && lhs.position == rhs.position
      && lhs.exponent == rhs.exponent
      && lhs.softness == rhs.softness;
}

enum class AudioSpectrumOrientation : std::uint8_t {
  Horizontal,
  Vertical,
};

struct AudioSpectrumStyle {
  Color color1{};
  Color color2{};
  AudioSpectrumOrientation orientation = AudioSpectrumOrientation::Horizontal;
  bool mirrored = false;
  bool centered = false;
};

constexpr bool operator==(const AudioSpectrumStyle& lhs, const AudioSpectrumStyle& rhs) noexcept {
  return lhs.color1 == rhs.color1
      && lhs.color2 == rhs.color2
      && lhs.orientation == rhs.orientation
      && lhs.mirrored == rhs.mirrored
      && lhs.centered == rhs.centered;
}

enum class FancyAudioVisualizerMode : std::uint8_t {
  Bars,
  Wave,
  Rings,
  BarsRings,
  WaveRings,
  All,
};

struct FancyAudioVisualizerStyle {
  Color primaryColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  Color secondaryColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  FancyAudioVisualizerMode mode = FancyAudioVisualizerMode::BarsRings;
  float time = 0.0f;
  float sensitivity = 1.5f;
  float rotationSpeed = 0.5f;
  float barWidth = 0.6f;
  float ringOpacity = 0.8f;
  float bloomIntensity = 0.5f;
  float waveThickness = 1.0f;
  float innerDiameter = 0.7f;
  float cornerRadius = 12.0f;
};

constexpr bool operator==(const FancyAudioVisualizerStyle& lhs, const FancyAudioVisualizerStyle& rhs) noexcept {
  return lhs.primaryColor == rhs.primaryColor
      && lhs.secondaryColor == rhs.secondaryColor
      && lhs.mode == rhs.mode
      && lhs.time == rhs.time
      && lhs.sensitivity == rhs.sensitivity
      && lhs.rotationSpeed == rhs.rotationSpeed
      && lhs.barWidth == rhs.barWidth
      && lhs.ringOpacity == rhs.ringOpacity
      && lhs.bloomIntensity == rhs.bloomIntensity
      && lhs.waveThickness == rhs.waveThickness
      && lhs.innerDiameter == rhs.innerDiameter
      && lhs.cornerRadius == rhs.cornerRadius;
}

enum class EffectType : std::uint8_t { None, Sun, Snow, Rain, Cloud, Fog, Stars };

struct EffectStyle {
  EffectType type = EffectType::None;
  float time = 0.0f;
  float radius = 0.0f;
  Color bgColor{};
};

struct GraphStyle {
  Color lineColor1{};
  float count1 = 0.0f;
  float scroll1 = 1.0f;

  Color lineColor2{};
  float count2 = 0.0f;
  float scroll2 = 1.0f;

  Color lineColor3{};
  float count3 = 0.0f;
  float scroll3 = 1.0f;

  float lineWidth = 1.5f;
  float graphFillOpacity = 0.15f;
  float aaSize = 0.5f;
};
