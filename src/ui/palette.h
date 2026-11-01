#pragma once

#include "render/core/color.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

enum class ColorRole : std::uint8_t {
  Primary,
  OnPrimary,
  PrimaryContainer,
  OnPrimaryContainer,
  Secondary,
  OnSecondary,
  SecondaryContainer,
  OnSecondaryContainer,
  Tertiary,
  OnTertiary,
  TertiaryContainer,
  OnTertiaryContainer,
  Error,
  OnError,
  ErrorContainer,
  OnErrorContainer,
  Surface,
  OnSurface,
  OnSurfaceVariant,
  SurfaceContainerLow,
  SurfaceContainer,
  SurfaceContainerHigh,
  SurfaceContainerHighest,
  OutlineVariant,
  Shadow,
  Scrim,
  Hover,
  OnHover,
};

struct ColorRoleToken {
  ColorRole role;
  std::string_view token;
};

inline constexpr std::array<ColorRoleToken, 28> kColorRoleTokens = {{
    {ColorRole::Primary, "primary"},
    {ColorRole::OnPrimary, "on_primary"},
    {ColorRole::PrimaryContainer, "primary_container"},
    {ColorRole::OnPrimaryContainer, "on_primary_container"},
    {ColorRole::Secondary, "secondary"},
    {ColorRole::OnSecondary, "on_secondary"},
    {ColorRole::SecondaryContainer, "secondary_container"},
    {ColorRole::OnSecondaryContainer, "on_secondary_container"},
    {ColorRole::Tertiary, "tertiary"},
    {ColorRole::OnTertiary, "on_tertiary"},
    {ColorRole::TertiaryContainer, "tertiary_container"},
    {ColorRole::OnTertiaryContainer, "on_tertiary_container"},
    {ColorRole::Error, "error"},
    {ColorRole::OnError, "on_error"},
    {ColorRole::ErrorContainer, "error_container"},
    {ColorRole::OnErrorContainer, "on_error_container"},
    {ColorRole::Surface, "surface"},
    {ColorRole::OnSurface, "on_surface"},
    {ColorRole::OnSurfaceVariant, "on_surface_variant"},
    {ColorRole::SurfaceContainerLow, "surface_container_low"},
    {ColorRole::SurfaceContainer, "surface_container"},
    {ColorRole::SurfaceContainerHigh, "surface_container_high"},
    {ColorRole::SurfaceContainerHighest, "surface_container_highest"},
    {ColorRole::OutlineVariant, "outline_variant"},
    {ColorRole::Shadow, "shadow"},
    {ColorRole::Scrim, "scrim"},
    {ColorRole::Hover, "hover"},
    {ColorRole::OnHover, "on_hover"},
}};

[[nodiscard]] constexpr Color clearColor() noexcept { return rgba(0.0F, 0.0F, 0.0F, 0.0F); }

struct ColorSpec {
  std::optional<ColorRole> role;
  Color fixed = clearColor();
  float alpha = 1.0F;
};

constexpr bool operator==(const ColorSpec& a, const ColorSpec& b) noexcept {
  return a.role == b.role && a.fixed == b.fixed && a.alpha == b.alpha;
}

[[nodiscard]] constexpr ColorSpec clearColorSpec() noexcept {
  return ColorSpec{.role = std::nullopt, .fixed = clearColor(), .alpha = 1.0F};
}

// Multiplies a spec's alpha, keeping any color role intact so theme switches still resolve.
// Distinct from withAlpha(Color, float) in render/core/color.h, which replaces the alpha of an
// already-resolved color and therefore cannot preserve the role.
[[nodiscard]] constexpr ColorSpec scaleAlpha(const ColorSpec& color, float factor) noexcept {
  ColorSpec out = color;
  out.alpha = std::clamp(out.alpha * std::clamp(factor, 0.0F, 1.0F), 0.0F, 1.0F);
  return out;
}

// Runtime UI palette: one color per ColorRole, resolved from the generated theme tokens.
struct Palette {
  Color primary;
  Color onPrimary;
  Color primaryContainer;
  Color onPrimaryContainer;
  Color secondary;
  Color onSecondary;
  Color secondaryContainer;
  Color onSecondaryContainer;
  Color tertiary;
  Color onTertiary;
  Color tertiaryContainer;
  Color onTertiaryContainer;
  Color error;
  Color onError;
  Color errorContainer;
  Color onErrorContainer;
  Color surface;
  Color onSurface;
  Color onSurfaceVariant;
  Color surfaceContainerLow;
  Color surfaceContainer;
  Color surfaceContainerHigh;
  Color surfaceContainerHighest;
  Color outlineVariant;
  Color shadow;
  Color scrim;
  Color hover;
  Color onHover;

  bool operator==(const Palette&) const = default;
};

extern Palette palette;

[[nodiscard]] const Color& colorForRole(ColorRole role) noexcept;
[[nodiscard]] Color colorForRole(ColorRole role, float alpha) noexcept;
[[nodiscard]] std::optional<ColorRole> colorRoleFromToken(std::string_view token);
// Plugin-facing lookup. Temporarily also accepts the retired `surface_variant` and `outline` roles (as
// surface_container and outline_variant, warning once per name) so third-party plugins keep working while
// they migrate. Remove in 5.5.0 and switch callers back to colorRoleFromToken().
[[nodiscard]] std::optional<ColorRole> colorRoleFromPluginToken(std::string_view token);
[[nodiscard]] std::string_view colorRoleToken(ColorRole role) noexcept;
[[nodiscard]] ColorSpec colorSpecFromRole(ColorRole role, float alpha = 1.0F) noexcept;
[[nodiscard]] ColorSpec fixedColorSpec(const Color& color) noexcept;
[[nodiscard]] Color resolveColorSpec(const ColorSpec& color) noexcept;
[[nodiscard]] bool isLightPalette() noexcept;
// Set by ThemeService from resolved light/dark mode (not inferred from palette colors).
[[nodiscard]] bool isResolvedLightTheme() noexcept;
void setResolvedThemeLight(bool light) noexcept;

void setPalette(const Palette& p);

// Snapshot accessor safe to call from non-main threads (e.g. plugin script workers).
[[nodiscard]] Color colorForRoleSnapshot(ColorRole role);

[[nodiscard]] inline ColorSpec scrollbarTrackColor() noexcept {
  return colorSpecFromRole(ColorRole::OutlineVariant, Style::disabledOutlineAlpha);
}
[[nodiscard]] inline ColorSpec scrollbarThumbColor() noexcept {
  return colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.5F);
}
[[nodiscard]] inline ColorSpec focusRingColorSpec() noexcept { return colorSpecFromRole(ColorRole::Secondary); }

// Fired after setPalette() writes. Controls subscribe in their constructor
// and re-apply palette-derived colors to their scene nodes on each emit.
Signal<>& paletteChanged();

// Linearly interpolates each field of two palettes in sRGB space. Used by
// ThemeService to drive smooth cross-fade transitions on theme changes.
Palette lerpPalette(const Palette& a, const Palette& b, float t);
