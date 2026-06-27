#pragma once

#include "render/core/color.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

enum class ColorRole : std::uint8_t {
  Primary,
  OnPrimary,
  Secondary,
  OnSecondary,
  Tertiary,
  OnTertiary,
  Error,
  OnError,
  Surface,
  OnSurface,
  SurfaceVariant,
  OnSurfaceVariant,
  Outline,
  Shadow,
  Hover,
  OnHover,
};

struct ColorRoleToken {
  ColorRole role;
  std::string_view token;
};

inline constexpr std::array<ColorRoleToken, 16> kColorRoleTokens = {{
    {ColorRole::Primary, "primary"},
    {ColorRole::OnPrimary, "on_primary"},
    {ColorRole::Secondary, "secondary"},
    {ColorRole::OnSecondary, "on_secondary"},
    {ColorRole::Tertiary, "tertiary"},
    {ColorRole::OnTertiary, "on_tertiary"},
    {ColorRole::Error, "error"},
    {ColorRole::OnError, "on_error"},
    {ColorRole::Surface, "surface"},
    {ColorRole::OnSurface, "on_surface"},
    {ColorRole::SurfaceVariant, "surface_variant"},
    {ColorRole::OnSurfaceVariant, "on_surface_variant"},
    {ColorRole::Outline, "outline"},
    {ColorRole::Shadow, "shadow"},
    {ColorRole::Hover, "hover"},
    {ColorRole::OnHover, "on_hover"},
}};

[[nodiscard]] constexpr Color clearColor() noexcept { return rgba(0.0f, 0.0f, 0.0f, 0.0f); }

struct ColorSpec {
  std::optional<ColorRole> role;
  Color fixed = clearColor();
  float alpha = 1.0f;
};

constexpr bool operator==(const ColorSpec& a, const ColorSpec& b) noexcept {
  return a.role == b.role && a.fixed == b.fixed && a.alpha == b.alpha;
}

[[nodiscard]] constexpr ColorSpec clearColorSpec() noexcept {
  return ColorSpec{.role = std::nullopt, .fixed = clearColor(), .alpha = 1.0f};
}

struct Palette {
  Color primary;
  Color onPrimary;
  Color secondary;
  Color onSecondary;
  Color tertiary;
  Color onTertiary;
  Color error;
  Color onError;
  Color surface;
  Color onSurface;
  Color surfaceVariant;
  Color onSurfaceVariant;
  Color outline;
  Color shadow;
  Color hover;
  Color onHover;
};

constexpr bool operator==(const Palette& lhs, const Palette& rhs) noexcept {
  return lhs.primary == rhs.primary
      && lhs.onPrimary == rhs.onPrimary
      && lhs.secondary == rhs.secondary
      && lhs.onSecondary == rhs.onSecondary
      && lhs.tertiary == rhs.tertiary
      && lhs.onTertiary == rhs.onTertiary
      && lhs.error == rhs.error
      && lhs.onError == rhs.onError
      && lhs.surface == rhs.surface
      && lhs.onSurface == rhs.onSurface
      && lhs.surfaceVariant == rhs.surfaceVariant
      && lhs.onSurfaceVariant == rhs.onSurfaceVariant
      && lhs.outline == rhs.outline
      && lhs.shadow == rhs.shadow
      && lhs.hover == rhs.hover
      && lhs.onHover == rhs.onHover;
}

extern Palette palette;

[[nodiscard]] const Color& colorForRole(ColorRole role) noexcept;
[[nodiscard]] Color colorForRole(ColorRole role, float alpha) noexcept;
[[nodiscard]] std::optional<ColorRole> colorRoleFromToken(std::string_view token);
[[nodiscard]] std::string_view colorRoleToken(ColorRole role) noexcept;
[[nodiscard]] ColorSpec colorSpecFromRole(ColorRole role, float alpha = 1.0f) noexcept;
[[nodiscard]] ColorSpec fixedColorSpec(const Color& color) noexcept;
[[nodiscard]] Color resolveColorSpec(const ColorSpec& color) noexcept;
[[nodiscard]] bool isLightPalette() noexcept;
// Set by ThemeService from resolved light/dark mode (not inferred from palette colors).
[[nodiscard]] bool isResolvedLightTheme() noexcept;
void setResolvedThemeLight(bool light) noexcept;

void setPalette(const Palette& p);

[[nodiscard]] inline ColorSpec scrollbarTrackColor() noexcept {
  return colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha);
}
[[nodiscard]] inline ColorSpec scrollbarThumbColor() noexcept {
  return colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.5f);
}
[[nodiscard]] inline ColorSpec focusRingColorSpec() noexcept { return colorSpecFromRole(ColorRole::Secondary); }

// Fired after setPalette() writes. Controls subscribe in their constructor
// and re-apply palette-derived colors to their scene nodes on each emit.
Signal<>& paletteChanged();

// Linearly interpolates each field of two palettes in sRGB space. Used by
// ThemeService to drive smooth cross-fade transitions on theme changes.
Palette lerpPalette(const Palette& a, const Palette& b, float t);
