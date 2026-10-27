#include "ui/palette.h"

#include "core/log.h"
#include "render/core/color.h"
#include "theme/builtin_palettes.h"
#include "util/string_utils.h"

#include <mutex>
#include <string>

Palette palette = noctalia::theme::mapGeneratedPaletteMode(
    noctalia::theme::expandBuiltinPalette(*noctalia::theme::findBuiltinPalette("Noctalia")).dark
);
bool g_resolvedThemeLight = false;

namespace {

  // Serializes setPalette() writes against colorForRoleSnapshot() reads from the
  // script runtime thread. All other palette reads (colorForRole, rendering) stay
  // on the main thread, so this holds only while setPalette() is main-thread-only.
  std::mutex g_paletteMutex;

  std::string normalizedRoleToken(std::string_view token) {
    std::string normalized = StringUtils::trim(token);
    StringUtils::toLowerInPlace(normalized);
    return normalized;
  }

  constexpr auto kLog = Logger("palette");

} // namespace

const Color& colorForRole(ColorRole role) noexcept {
  switch (role) {
  case ColorRole::Primary:
    return palette.primary;
  case ColorRole::OnPrimary:
    return palette.onPrimary;
  case ColorRole::PrimaryContainer:
    return palette.primaryContainer;
  case ColorRole::OnPrimaryContainer:
    return palette.onPrimaryContainer;
  case ColorRole::Secondary:
    return palette.secondary;
  case ColorRole::OnSecondary:
    return palette.onSecondary;
  case ColorRole::SecondaryContainer:
    return palette.secondaryContainer;
  case ColorRole::OnSecondaryContainer:
    return palette.onSecondaryContainer;
  case ColorRole::Tertiary:
    return palette.tertiary;
  case ColorRole::OnTertiary:
    return palette.onTertiary;
  case ColorRole::TertiaryContainer:
    return palette.tertiaryContainer;
  case ColorRole::OnTertiaryContainer:
    return palette.onTertiaryContainer;
  case ColorRole::Error:
    return palette.error;
  case ColorRole::OnError:
    return palette.onError;
  case ColorRole::ErrorContainer:
    return palette.errorContainer;
  case ColorRole::OnErrorContainer:
    return palette.onErrorContainer;
  case ColorRole::Surface:
    return palette.surface;
  case ColorRole::OnSurface:
    return palette.onSurface;
  case ColorRole::OnSurfaceVariant:
    return palette.onSurfaceVariant;
  case ColorRole::SurfaceContainerLow:
    return palette.surfaceContainerLow;
  case ColorRole::SurfaceContainer:
    return palette.surfaceContainer;
  case ColorRole::SurfaceContainerHigh:
    return palette.surfaceContainerHigh;
  case ColorRole::SurfaceContainerHighest:
    return palette.surfaceContainerHighest;
  case ColorRole::OutlineVariant:
    return palette.outlineVariant;
  case ColorRole::Shadow:
    return palette.shadow;
  case ColorRole::Scrim:
    return palette.scrim;
  case ColorRole::Hover:
    return palette.hover;
  case ColorRole::OnHover:
    return palette.onHover;
  }

  return palette.onSurface;
}

Color colorForRole(ColorRole role, float alpha) noexcept {
  Color color = colorForRole(role);
  color.a *= alpha;
  return color;
}

std::optional<ColorRole> colorRoleFromToken(std::string_view token) {
  const std::string normalized = normalizedRoleToken(token);
  for (const auto& option : kColorRoleTokens) {
    if (normalized == option.token) {
      return option.role;
    }
  }
  return std::nullopt;
}

std::optional<ColorRole> colorRoleFromPluginToken(std::string_view token) {
  if (auto role = colorRoleFromToken(token)) {
    return role;
  }
  const std::string normalized = normalizedRoleToken(token);
  if (normalized == "surface_variant") {
    static std::once_flag warned;
    std::call_once(warned, [] {
      kLog.warn(R"(plugin color role "surface_variant" is deprecated; use "surface_container")");
    });
    return ColorRole::SurfaceContainer;
  }
  if (normalized == "outline") {
    static std::once_flag warned;
    std::call_once(warned, [] { kLog.warn(R"(plugin color role "outline" is deprecated; use "outline_variant")"); });
    return ColorRole::OutlineVariant;
  }
  return std::nullopt;
}

std::string_view colorRoleToken(ColorRole role) noexcept {
  for (const auto& option : kColorRoleTokens) {
    if (option.role == role) {
      return option.token;
    }
  }
  return "on_surface";
}

ColorSpec colorSpecFromRole(ColorRole role, float alpha) noexcept {
  return ColorSpec{.role = role, .fixed = clearColor(), .alpha = alpha};
}

ColorSpec fixedColorSpec(const Color& color) noexcept {
  return ColorSpec{.role = std::nullopt, .fixed = color, .alpha = 1.0F};
}

Color resolveColorSpec(const ColorSpec& color) noexcept {
  Color resolved = color.role.has_value() ? colorForRole(*color.role) : color.fixed;
  resolved.a *= color.alpha;
  return resolved;
}

bool isLightPalette() noexcept { return relativeLuminance(palette.surface) > 0.179F; }

bool isResolvedLightTheme() noexcept { return g_resolvedThemeLight; }

void setResolvedThemeLight(bool light) noexcept { g_resolvedThemeLight = light; }

Signal<>& paletteChanged() {
  static Signal<> signal;
  return signal;
}

void setPalette(const Palette& p) {
  {
    std::scoped_lock lock(g_paletteMutex);
    if (palette == p) {
      return;
    }
    palette = p;
  }
  paletteChanged().emit();
}

Color colorForRoleSnapshot(ColorRole role) {
  std::scoped_lock lock(g_paletteMutex);
  return colorForRole(role);
}

Palette lerpPalette(const Palette& a, const Palette& b, float t) {
  return Palette{
      .primary = lerpColor(a.primary, b.primary, t),
      .onPrimary = lerpColor(a.onPrimary, b.onPrimary, t),
      .primaryContainer = lerpColor(a.primaryContainer, b.primaryContainer, t),
      .onPrimaryContainer = lerpColor(a.onPrimaryContainer, b.onPrimaryContainer, t),
      .secondary = lerpColor(a.secondary, b.secondary, t),
      .onSecondary = lerpColor(a.onSecondary, b.onSecondary, t),
      .secondaryContainer = lerpColor(a.secondaryContainer, b.secondaryContainer, t),
      .onSecondaryContainer = lerpColor(a.onSecondaryContainer, b.onSecondaryContainer, t),
      .tertiary = lerpColor(a.tertiary, b.tertiary, t),
      .onTertiary = lerpColor(a.onTertiary, b.onTertiary, t),
      .tertiaryContainer = lerpColor(a.tertiaryContainer, b.tertiaryContainer, t),
      .onTertiaryContainer = lerpColor(a.onTertiaryContainer, b.onTertiaryContainer, t),
      .error = lerpColor(a.error, b.error, t),
      .onError = lerpColor(a.onError, b.onError, t),
      .errorContainer = lerpColor(a.errorContainer, b.errorContainer, t),
      .onErrorContainer = lerpColor(a.onErrorContainer, b.onErrorContainer, t),
      .surface = lerpColor(a.surface, b.surface, t),
      .onSurface = lerpColor(a.onSurface, b.onSurface, t),
      .onSurfaceVariant = lerpColor(a.onSurfaceVariant, b.onSurfaceVariant, t),
      .surfaceContainerLow = lerpColor(a.surfaceContainerLow, b.surfaceContainerLow, t),
      .surfaceContainer = lerpColor(a.surfaceContainer, b.surfaceContainer, t),
      .surfaceContainerHigh = lerpColor(a.surfaceContainerHigh, b.surfaceContainerHigh, t),
      .surfaceContainerHighest = lerpColor(a.surfaceContainerHighest, b.surfaceContainerHighest, t),
      .outlineVariant = lerpColor(a.outlineVariant, b.outlineVariant, t),
      .shadow = lerpColor(a.shadow, b.shadow, t),
      .scrim = lerpColor(a.scrim, b.scrim, t),
      .hover = lerpColor(a.hover, b.hover, t),
      .onHover = lerpColor(a.onHover, b.onHover, t),
  };
}
