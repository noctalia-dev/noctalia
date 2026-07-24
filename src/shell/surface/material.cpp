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
        const float clearAlpha = std::lerp(0.55f, 0.68f, L);
        const float denseAlpha = std::lerp(0.78f, 0.90f, L);
        return std::lerp(clearAlpha, denseAlpha, d);
      }
      case SurfaceMaterialMode::LiquidGlass: {
        const float clearAlpha = std::lerp(0.05f, 0.09f, L);
        const float denseAlpha = std::lerp(0.16f, 0.28f, L);
        return std::lerp(clearAlpha, denseAlpha, d);
      }
      }
      return d;
    }

    void applyLiquidGlass(
        RoundedRectStyle& style, float density, float bodyAlpha, float surfaceLuminance, LightEdge lightEdge,
        bool hasExplicitBorder, const ColorSpec& border, float borderWidth, bool refractionActive
    ) {
      const Color surface = colorForRole(ColorRole::Surface);
      const float L = clamp01(surfaceLuminance);
      const float d = clamp01(density);

      const Color glassTint = lerpColor(rgba(1.0f, 1.0f, 1.0f, 1.0f), surface, std::lerp(0.08f, 0.22f, L));
      // Refraction-active: slightly more open mid + stronger outer specular (lens cue).
      // Default (refraction off) clamp maxima match the pre-refraction liquid-glass recipe.
      const float outerBoost = refractionActive ? 1.75f : 1.55f;
      const float innerScale = refractionActive ? 0.35f : 0.45f;
      const float outerMax = refractionActive ? 0.48f : 0.42f;
      const float outerAlpha = clamp01(bodyAlpha * outerBoost);
      const float midAlpha = bodyAlpha;
      const float innerAlpha = clamp01(bodyAlpha * innerScale);
      const Color outer = withAlpha(glassTint, std::clamp(outerAlpha, 0.08f, outerMax));
      const Color mid = withAlpha(glassTint, midAlpha);
      const Color inner = withAlpha(glassTint, std::clamp(innerAlpha, 0.02f, 0.18f));

      style.fill = mid;
      style.fillMode = FillMode::LinearGradient;
      style.softness = refractionActive ? 1.05f : 0.65f;

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

      // hasExplicitBorder alone wins (including width 0 = seamless / borderless chrome).
      if (hasExplicitBorder) {
        style.border = borderWidth > 0.0f ? resolveColorSpec(border) : clearColor();
        style.borderWidth = borderWidth;
      } else {
        const float rimBase = refractionActive ? std::lerp(0.50f, 0.78f, d) : std::lerp(0.42f, 0.62f, d);
        const float rimMax = refractionActive ? 0.85f : 0.75f;
        const float rimAlpha = std::clamp(rimBase * std::lerp(1.0f, 0.55f, L), 0.2f, rimMax);
        style.border = withAlpha(rgba(1.0f, 1.0f, 1.0f, 1.0f), rimAlpha);
        style.borderWidth = refractionActive ? 1.5f : 1.25f;
      }
    }

    void applySoft(
        RoundedRectStyle& style, float bodyAlpha, bool hasExplicitBorder, const ColorSpec& border, float borderWidth
    ) {
      style.fill = colorForRole(ColorRole::Surface, bodyAlpha);
      style.fillMode = FillMode::Solid;
      style.softness = 0.35f;
      // Match solid: explicit border (including width 0) overrides the material rim.
      if (hasExplicitBorder) {
        style.border = borderWidth > 0.0f ? resolveColorSpec(border) : clearColor();
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

  SurfaceMaterialMode
  effectiveMode(const ShellConfig& shell, std::optional<SurfaceMaterialMode> surfaceOverride) noexcept {
    if (surfaceOverride.has_value()) {
      return *surfaceOverride;
    }
    return shell.material.mode;
  }

  bool refractionRequested(const Params& params) noexcept {
    return params.experimentalRefraction && params.mode == SurfaceMaterialMode::LiquidGlass;
  }

  bool refractionActive(const Params& params) noexcept {
    return refractionRequested(params) && params.refractionSamplingAvailable;
  }

  Params fromShell(
      const ShellConfig& shell, float surfaceOpacity, std::optional<SurfaceMaterialMode> modeOverride
  ) noexcept {
    Params params;
    params.mode = effectiveMode(shell, modeOverride);
    params.experimentalRefraction = shell.material.experimentalRefraction;
    // Client-side lens refraction is paint-only and always available when the experimental
    // flag is on. Desktop/screencopy sampling can still be forced off by callers for fallback
    // testing; production fromShell enables the attempt whenever the flag is set.
    params.refractionSamplingAvailable = shell.material.experimentalRefraction;
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
    out.refractionRequested = refractionRequested(params);
    out.refractionActive = refractionActive(params);
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
          params.border, params.borderWidth, resolved.refractionActive
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
      const Color surface = colorForRole(ColorRole::Surface);
      const float L = clamp01(surfaceRelativeLuminance);
      const Color glassTint = lerpColor(rgba(1.0f, 1.0f, 1.0f, 1.0f), surface, std::lerp(0.08f, 0.22f, L));
      return fixedColorSpec(withAlpha(glassTint, resolved.bodyAlpha));
    }
    return colorSpecFromRole(ColorRole::Surface, resolved.bodyAlpha);
  }

} // namespace shell::material
