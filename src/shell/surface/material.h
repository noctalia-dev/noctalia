#pragma once

#include "config/config_types.h"
#include "render/core/render_styles.h"
#include "ui/palette.h"

#include <string_view>

// Shared surface material engine: solid / soft / liquid-glass look for any chrome
// (bar, dock, panels, popups). Compositor blur is still published by each surface;
// this module owns the painted fill + specular rim recipe.
namespace shell::material {

  // Which outer edge faces the screen edge / ambient light. Used by liquid glass
  // to put the specular catch-light on the correct side of the shape.
  enum class LightEdge : std::uint8_t {
    Top = 0,
    Bottom = 1,
    Left = 2,
    Right = 3,
    Ambient = 4, // floating surfaces: soft top-weighted wash
  };

  struct Params {
    SurfaceMaterialMode mode = SurfaceMaterialMode::Solid;
    // 0..1. Solid: raw body alpha. Soft / liquid glass: material density.
    float density = 1.0f;
    // When true, caller-supplied border wins over the material rim.
    bool hasExplicitBorder = false;
    ColorSpec border = colorSpecFromRole(ColorRole::Outline);
    float borderWidth = 0.0f;
  };

  struct Resolved {
    SurfaceMaterialMode mode = SurfaceMaterialMode::Solid;
    float density = 1.0f;
    // Body alpha for shadows, attached-panel tracking, and solid fills.
    float bodyAlpha = 1.0f;
  };

  [[nodiscard]] LightEdge lightEdgeFromBarPosition(std::string_view position) noexcept;

  // density = global material density * surface opacity (both clamped).
  [[nodiscard]] Params fromShell(const ShellConfig& shell, float surfaceOpacity = 1.0f) noexcept;

  [[nodiscard]] Resolved resolve(const Params& params) noexcept;
  [[nodiscard]] Resolved resolve(const Params& params, float surfaceRelativeLuminance) noexcept;

  // Writes fill / gradient / border / softness. Preserves radius, corners, insets.
  void applyToStyle(RoundedRectStyle& style, const Params& params, LightEdge lightEdge = LightEdge::Ambient);
  void applyToStyle(
      RoundedRectStyle& style, const Params& params, LightEdge lightEdge, float surfaceRelativeLuminance
  );

  // Convenience for simple solid/soft cards that only need a Surface role fill.
  [[nodiscard]] ColorSpec fillSpec(const Params& params) noexcept;
  [[nodiscard]] ColorSpec fillSpec(const Params& params, float surfaceRelativeLuminance) noexcept;

} // namespace shell::material
