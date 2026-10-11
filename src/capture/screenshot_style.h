#pragma once

#include "capture/screencopy_capture.h"
#include "render/core/color.h"
#include "ui/palette.h"

#include <cstdint>
#include <optional>

namespace capture {

  // Resolved presentation for a styled screenshot: the capture sits on a padded canvas with
  // rounded corners, an optional soft drop shadow and an optional linear-gradient backdrop.
  // All lengths are in image pixels.
  struct ScreenshotStyle {
    bool background = true;
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

    bool operator==(const ScreenshotStyle&) const = default;
  };

  // Editable style as the annotation editor holds it: colors may be theme roles, resolved at use.
  struct ScreenshotStyleSettings {
    bool enabled = false;
    bool background = true;
    ColorSpec gradientStart = colorSpecFromRole(ColorRole::Primary);
    ColorSpec gradientEnd = colorSpecFromRole(ColorRole::Tertiary);
    float gradientAngle = 135.0F;
    std::int32_t padding = 64;
    std::int32_t cornerRadius = 12;
    bool shadow = true;
    ColorSpec shadowColor = colorSpecFromRole(ColorRole::Shadow);
    float shadowOpacity = 0.5F;
    std::int32_t shadowBlur = 40;
    std::int32_t shadowOffsetX = 0;
    std::int32_t shadowOffsetY = 16;

    bool operator==(const ScreenshotStyleSettings&) const = default;
  };

  // Resolves theme roles against the current palette. Returns nullopt when styling is disabled.
  [[nodiscard]] std::optional<ScreenshotStyle> resolveScreenshotStyle(const ScreenshotStyleSettings& settings);

  // Returns the styled copy of a top-down, straight-alpha RGBA image. An empty source is returned
  // unchanged.
  [[nodiscard]] ScreencopyImage applyScreenshotStyle(const ScreencopyImage& source, const ScreenshotStyle& style);

} // namespace capture
