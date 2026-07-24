#include "shell/surface/material.h"

#include "render/core/color.h"

#include <algorithm>
#include <cmath>

namespace shell::material {
  namespace {

    [[nodiscard]] float clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }

    [[nodiscard]] float bodyAlphaForMode(SurfaceMaterialMode mode, float density, float surfaceLuminance) noexcept {
      const float d = clamp01(density);
      const float L = clamp01(surfaceLuminance);
      switch (mode) {
      case SurfaceMaterialMode::Solid:
        return d;
      case SurfaceMaterialMode::Soft: {
        // Light frosted plate: denser than liquid glass, still translucent.
        const float clearAlpha = std::lerp(0.55f, 0.68f, L);
        const float denseAlpha = std::lerp(0.78f, 0.90f, L);
        return std::lerp(clearAlpha, denseAlpha, d);
      }
      case SurfaceMaterialMode::LiquidGlass: {
        // Clear liquid glass (Control Center style): thin refractive wash, never milky.
        const float clearAlpha = std::lerp(0.05f, 0.09f, L);
        const float denseAlpha = std::lerp(0.16f, 0.28f, L);
        return std::lerp(clearAlpha, denseAlpha, d);
      }
      }
      return d;
    }

    void applyLiquidGlass(
        RoundedRectStyle& style, float density, float bodyAlpha, float surfaceLuminance, LightEdge lightEdge,
        bool hasExplicitBorder, const ColorSpec& border, float borderWidth
    ) {
      const Color surface = colorForRole(ColorRole::Surface);
      const float L = clamp01(surfaceLuminance);
      const float d = clamp01(density);

      // Mostly white so the backdrop shows through; slight Surface mix keeps the palette present.
      const Color glassTint = lerpColor(rgba(1.0f, 1.0f, 1.0f, 1.0f), surface, std::lerp(0.08f, 0.22f, L));
      const float outerAlpha = clamp01(bodyAlpha * 1.55f);
      const float midAlpha = bodyAlpha;
      const float innerAlpha = clamp01(bodyAlpha * 0.45f);
      const Color outer = withAlpha(glassTint, std::clamp(outerAlpha, 0.08f, 0.42f));
      const Color mid = withAlpha(glassTint, midAlpha);
      const Color inner = withAlpha(glassTint, std::clamp(innerAlpha, 0.02f, 0.18f));

      style.fill = mid;
      style.fillMode = FillMode::LinearGradient;
      style.softness = 0.65f;

      switch (lightEdge) {
      case LightEdge::Left:
        style.gradientDirection = GradientDirection::Horizontal;
        style.gradientStops = {
            GradientStop{0.0f, outer}, GradientStop{0.28f, mid}, GradientStop{1.0f, inner}, GradientStop{1.0f, inner}
        };
        break;
      case LightEdge::Right:
        style.gradientDirection = GradientDirection::Horizontal;
        style.gradientStops = {
            GradientStop{0.0f, inner}, GradientStop{0.72f, mid}, GradientStop{1.0f, outer}, GradientStop{1.0f, outer}
        };
        break;
      case LightEdge::Bottom:
        style.gradientDirection = GradientDirection::Vertical;
        style.gradientStops = {
            GradientStop{0.0f, inner}, GradientStop{0.72f, mid}, GradientStop{1.0f, outer}, GradientStop{1.0f, outer}
        };
        break;
      case LightEdge::Top:
      case LightEdge::Ambient:
      default:
        style.gradientDirection = GradientDirection::Vertical;
        style.gradientStops = {
            GradientStop{0.0f, outer}, GradientStop{0.28f, mid}, GradientStop{1.0f, inner}, GradientStop{1.0f, inner}
        };
        break;
      }

      if (hasExplicitBorder && borderWidth > 0.0f) {
        style.border = resolveColorSpec(border);
        style.borderWidth = borderWidth;
      } else {
        const float rimAlpha = std::clamp(std::lerp(0.42f, 0.62f, d) * std::lerp(1.0f, 0.55f, L), 0.2f, 0.75f);
        style.border = withAlpha(rgba(1.0f, 1.0f, 1.0f, 1.0f), rimAlpha);
        style.borderWidth = 1.25f;
      }
    }

    void applySoft(
        RoundedRectStyle& style, float bodyAlpha, bool hasExplicitBorder, const ColorSpec& border, float borderWidth
    ) {
      style.fill = colorForRole(ColorRole::Surface, bodyAlpha);
      style.fillMode = FillMode::Solid;
      style.softness = 0.35f;
      if (hasExplicitBorder && borderWidth > 0.0f) {
        style.border = resolveColorSpec(border);
        style.borderWidth = borderWidth;
      } else {
        style.border = colorForRole(ColorRole::Outline, 0.28f);
        style.borderWidth = 1.0f;
      }
    }

    void applySolid(
        RoundedRectStyle& style, float bodyAlpha, bool hasExplicitBorder, const ColorSpec& border, float borderWidth
    ) {
      style.fill = colorForRole(ColorRole::Surface, bodyAlpha);
      style.fillMode = FillMode::Solid;
      style.softness = 0.0f;
      if (hasExplicitBorder) {
        style.border = resolveColorSpec(border);
        style.borderWidth = borderWidth;
      } else {
        style.border = clearColor();
        style.borderWidth = 0.0f;
      }
    }

  } // namespace

  LightEdge lightEdgeFromBarPosition(std::string_view position) noexcept {
    if (position == "bottom") {
      return LightEdge::Bottom;
    }
    if (position == "left") {
      return LightEdge::Left;
    }
    if (position == "right") {
      return LightEdge::Right;
    }
    return LightEdge::Top;
  }

  Params fromShell(const ShellConfig& shell, float surfaceOpacity) noexcept {
    Params params;
    params.mode = shell.material.mode;
    // Solid ignores master density so background_opacity stays a raw alpha.
    // Soft / liquid glass multiply global density by the surface's opacity slider.
    if (params.mode == SurfaceMaterialMode::Solid) {
      params.density = clamp01(surfaceOpacity);
    } else {
      params.density = clamp01(shell.material.density) * clamp01(surfaceOpacity);
    }
    return params;
  }

  Resolved resolve(const Params& params) noexcept {
    return resolve(params, relativeLuminance(colorForRole(ColorRole::Surface)));
  }

  Resolved resolve(const Params& params, float surfaceRelativeLuminance) noexcept {
    Resolved out;
    out.mode = params.mode;
    out.density = clamp01(params.density);
    out.bodyAlpha = bodyAlphaForMode(params.mode, out.density, surfaceRelativeLuminance);
    return out;
  }

  void applyToStyle(RoundedRectStyle& style, const Params& params, LightEdge lightEdge) {
    applyToStyle(style, params, lightEdge, relativeLuminance(colorForRole(ColorRole::Surface)));
  }

  void applyToStyle(
      RoundedRectStyle& style, const Params& params, LightEdge lightEdge, float surfaceRelativeLuminance
  ) {
    const Resolved resolved = resolve(params, surfaceRelativeLuminance);
    switch (resolved.mode) {
    case SurfaceMaterialMode::LiquidGlass:
      applyLiquidGlass(
          style, resolved.density, resolved.bodyAlpha, surfaceRelativeLuminance, lightEdge, params.hasExplicitBorder,
          params.border, params.borderWidth
      );
      break;
    case SurfaceMaterialMode::Soft:
      applySoft(style, resolved.bodyAlpha, params.hasExplicitBorder, params.border, params.borderWidth);
      break;
    case SurfaceMaterialMode::Solid:
    default:
      applySolid(style, resolved.bodyAlpha, params.hasExplicitBorder, params.border, params.borderWidth);
      break;
    }
  }

  ColorSpec fillSpec(const Params& params) noexcept {
    return fillSpec(params, relativeLuminance(colorForRole(ColorRole::Surface)));
  }

  ColorSpec fillSpec(const Params& params, float surfaceRelativeLuminance) noexcept {
    const Resolved resolved = resolve(params, surfaceRelativeLuminance);
    if (resolved.mode == SurfaceMaterialMode::LiquidGlass) {
      // Approximate liquid-glass body as a white-tinted Surface for callers that only
      // accept a ColorSpec (cards, shadows). Full style needs applyToStyle.
      const Color surface = colorForRole(ColorRole::Surface);
      const float L = clamp01(surfaceRelativeLuminance);
      const Color glassTint = lerpColor(rgba(1.0f, 1.0f, 1.0f, 1.0f), surface, std::lerp(0.08f, 0.22f, L));
      return fixedColorSpec(withAlpha(glassTint, resolved.bodyAlpha));
    }
    return colorSpecFromRole(ColorRole::Surface, resolved.bodyAlpha);
  }

} // namespace shell::material
