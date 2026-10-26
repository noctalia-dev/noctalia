#pragma once

#include "capture/screencopy_capture.h"
#include "render/core/color.h"
#include "ui/palette.h"

#include <cstdint>
#include <optional>

namespace capture {

  // Resolved presentation for a screenshot with a backdrop: the capture sits on a padded canvas with
  // rounded corners, an optional soft drop shadow and an optional linear-gradient fill.
  // All lengths are in image pixels.
  struct ScreenshotBackdrop {
    bool gradient = true;
    Color gradientStart;
    Color gradientEnd;
    float gradientAngleDegrees = 135.0F; // CSS convention: 90 = left→right, 180 = top→bottom
    int padding = 64;
    int cornerRadius = 12;
    bool shadow = true;
    Color shadowColor;
    float shadowOpacity = 0.5F;
    int shadowBlur = 40;
    int shadowOffsetX = 0;
    int shadowOffsetY = 16;

    bool operator==(const ScreenshotBackdrop&) const = default;
  };

  // Editable backdrop as the annotation editor holds it: colors may be theme roles, resolved at use.
  struct ScreenshotBackdropSettings {
    bool enabled = false;
    bool gradient = true;
    ColorSpec gradientStart = colorSpecFromRole(ColorRole::Primary);
    ColorSpec gradientEnd = colorSpecFromRole(ColorRole::Tertiary);
    float gradientAngle = 135.0F;
    std::int32_t padding = 64;
    std::int32_t cornerRadius = 12;
    bool shadow = true;
    ColorSpec shadowColor = colorSpecFromRole(ColorRole::Shadow);
    float shadowOpacity = 0.5F;
    std::int32_t shadowBlur = 40;
    std::int32_t shadowOffsetX = 16;
    std::int32_t shadowOffsetY = 16;

    bool operator==(const ScreenshotBackdropSettings&) const = default;
  };

  // Resolves theme roles against the current palette. Returns nullopt when the backdrop is disabled.
  [[nodiscard]] std::optional<ScreenshotBackdrop> resolveScreenshotBackdrop(const ScreenshotBackdropSettings& config);

  // Returns a copy of a top-down, straight-alpha RGBA image with the backdrop applied. An empty source is
  // returned unchanged.
  [[nodiscard]] ScreencopyImage
  applyScreenshotBackdrop(const ScreencopyImage& source, const ScreenshotBackdrop& backdrop);

} // namespace capture
