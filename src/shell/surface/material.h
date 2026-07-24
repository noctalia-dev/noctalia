#pragma once

#include "config/config_types.h"
#include "render/core/render_styles.h"
#include "ui/palette.h"

#include <optional>
#include <string_view>

// Shared surface material engine: solid / soft / liquid-glass look for any chrome
// (bar, dock, panels, popups, OSD, toasts). Compositor blur is still published by
// each surface; this module owns the painted fill + specular rim recipe and the
// pure resolve path for mode/density/overrides/refraction gating.
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
    // From shell.material.experimental_refraction — enables the client-side lens attempt.
    bool experimentalRefraction = false;
    // When true with experimentalRefraction + liquid_glass, applyToStyle uses the stronger
    // lens recipe. fromShell sets this true whenever the experimental flag is on (client-side
    // lens is always available). Set false to force the plain liquid-glass paint fallback.
    bool refractionSamplingAvailable = false;
  };

  struct Resolved {
    SurfaceMaterialMode mode = SurfaceMaterialMode::Solid;
    float density = 1.0f;
    // Body alpha for shadows, attached-panel tracking, and solid fills.
    float bodyAlpha = 1.0f;
    // Flag on and mode supports refraction (liquid glass).
    bool refractionRequested = false;
    // Requested and sampling available — full lens path may run; otherwise paint fallback.
    bool refractionActive = false;
  };

  [[nodiscard]] LightEdge lightEdgeFromBarPosition(std::string_view position) noexcept;

  // Prefer surface override when set; otherwise global shell.material.mode.
  [[nodiscard]] SurfaceMaterialMode
  effectiveMode(const ShellConfig& shell, std::optional<SurfaceMaterialMode> surfaceOverride = std::nullopt) noexcept;

  // Pure refraction gating (no GL/Wayland). Used by unit tests and applyToStyle.
  [[nodiscard]] bool refractionRequested(const Params& params) noexcept;
  [[nodiscard]] bool refractionActive(const Params& params) noexcept;

  // density from global material density * surface opacity (solid ignores master density).
  [[nodiscard]] Params fromShell(
      const ShellConfig& shell, float surfaceOpacity = 1.0f,
      std::optional<SurfaceMaterialMode> modeOverride = std::nullopt
  ) noexcept;

  [[nodiscard]] Resolved resolve(const Params& params) noexcept;
  [[nodiscard]] Resolved resolve(const Params& params, float surfaceRelativeLuminance) noexcept;

  // Writes fill / gradient / border / softness. Preserves radius, corners, insets.
  // When experimental refraction is on but sampling is unavailable, still applies the
  // liquid-glass recipe (safe fallback); when sampling is available, applies a stronger
  // lens-style rim as the client-side refraction attempt without requiring GL here.
  void applyToStyle(RoundedRectStyle& style, const Params& params, LightEdge lightEdge = LightEdge::Ambient);
  void applyToStyle(
      RoundedRectStyle& style, const Params& params, LightEdge lightEdge, float surfaceRelativeLuminance
  );

  // Convenience for simple solid/soft cards that only need a Surface role fill.
  [[nodiscard]] ColorSpec fillSpec(const Params& params) noexcept;
  [[nodiscard]] ColorSpec fillSpec(const Params& params, float surfaceRelativeLuminance) noexcept;

} // namespace shell::material
