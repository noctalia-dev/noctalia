#include "theme/fixed_palette.h"

#include "render/core/color.h"
#include "theme/color.h"
#include "theme/contrast.h"

#include <algorithm>
#include <tuple>

namespace noctalia::theme {

  namespace {

    constexpr std::uint32_t kOpaqueBlack = 0xff000000U;
    constexpr std::uint32_t kOpaqueWhite = 0xffffffffU;

    ::Color tokenToColor(const TokenMap& tokens, std::string_view key) {
      auto it = tokens.find(std::string(key));
      if (it == tokens.end())
        return hex("#ff00ff");
      return rgbHex(it->second & 0x00FFFFFFU);
    }

    Color toUiColor(const ::Color& color) {
      auto toByte = [](float value) { return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
      return Color(toByte(color.r), toByte(color.g), toByte(color.b));
    }

    void setToken(TokenMap& dst, std::string_view key, const Color& color) { dst[std::string(key)] = color.toArgb(); }

    std::uint32_t colorToArgb(const ::Color& color) {
      auto toByte = [](float value) {
        return static_cast<std::uint32_t>(value <= 0.0f ? 0.0f : (value >= 1.0f ? 255.0f : value * 255.0f + 0.5f));
      };
      return kOpaqueBlack | (toByte(color.r) << 16U) | (toByte(color.g) << 8U) | toByte(color.b);
    }

    void setToken(TokenMap& dst, std::string_view key, const ::Color& color) {
      dst[std::string(key)] = colorToArgb(color);
    }

    bool hasToken(const TokenMap& tokens, std::string_view key) { return tokens.contains(std::string(key)); }

    std::uint32_t tokenOr(const TokenMap& tokens, std::string_view key, std::uint32_t fallback) {
      auto it = tokens.find(std::string(key));
      return it == tokens.end() ? fallback : it->second;
    }

    void setMissingToken(TokenMap& tokens, std::string_view key, std::uint32_t value) {
      if (!hasToken(tokens, key)) {
        tokens[std::string(key)] = value;
      }
    }

    const ::Color& ansiColorForKey(const TerminalAnsiColors& colors, std::string_view key) {
      if (key == kTerminalBlackJsonKey)
        return colors.black;
      if (key == kTerminalRedJsonKey)
        return colors.red;
      if (key == kTerminalGreenJsonKey)
        return colors.green;
      if (key == kTerminalYellowJsonKey)
        return colors.yellow;
      if (key == kTerminalBlueJsonKey)
        return colors.blue;
      if (key == kTerminalMagentaJsonKey)
        return colors.magenta;
      if (key == kTerminalCyanJsonKey)
        return colors.cyan;
      return colors.white;
    }

    const ::Color& directColorForKey(const TerminalPalette& terminal, std::string_view key) {
      if (key == kTerminalForegroundJsonKey)
        return terminal.foreground;
      if (key == kTerminalBackgroundJsonKey)
        return terminal.background;
      if (key == kTerminalCursorJsonKey)
        return terminal.cursor;
      if (key == kTerminalCursorTextJsonKey)
        return terminal.cursorText;
      if (key == kTerminalSelectionFgJsonKey)
        return terminal.selectionFg;
      return terminal.selectionBg;
    }

    const TerminalAnsiColors& ansiGroupForKey(const TerminalPalette& terminal, std::string_view key) {
      return key == kTerminalBrightJsonKey ? terminal.bright : terminal.normal;
    }

    void applyAnsiColors(TokenMap& tokens, std::string_view prefix, const TerminalAnsiColors& colors) {
      for (const auto key : kTerminalAnsiColorJsonKeys) {
        std::string tokenKey(prefix);
        tokenKey += "_";
        tokenKey += key;
        setToken(tokens, tokenKey, ansiColorForKey(colors, key));
      }
    }

    Color interpolateColor(const Color& a, const Color& b, double t) {
      auto mix = [t](int lhs, int rhs) { return static_cast<int>(lhs + (rhs - lhs) * t); };
      return Color(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
    }

  } // namespace

  void applyTerminalPalette(TokenMap& tokens, const TerminalPalette& terminal) {
    for (const auto& group : kTerminalAnsiGroupTokenKeys) {
      applyAnsiColors(tokens, group.tokenPrefix, ansiGroupForKey(terminal, group.jsonKey));
    }
    for (const auto& key : kTerminalDirectColorTokenKeys) {
      setToken(tokens, key.tokenKey, directColorForKey(terminal, key.jsonKey));
    }
  }

  void synthesizeTerminalPaletteTokens(TokenMap& tokens) {
    const std::uint32_t background = tokenOr(tokens, "background", tokenOr(tokens, "surface", kOpaqueBlack));
    const std::uint32_t foreground = tokenOr(tokens, "on_surface", kOpaqueWhite);
    const std::uint32_t surface = tokenOr(tokens, "surface", background);
    const std::uint32_t surfaceVariant = tokenOr(tokens, "surface_variant", surface);
    const std::uint32_t onSurfaceVariant = tokenOr(tokens, "on_surface_variant", foreground);
    const std::uint32_t outline = tokenOr(tokens, "outline", onSurfaceVariant);
    const std::uint32_t error = tokenOr(tokens, "error", foreground);
    const std::uint32_t primary = tokenOr(tokens, "primary", foreground);
    const std::uint32_t secondary = tokenOr(tokens, "secondary", primary);
    const std::uint32_t tertiary = tokenOr(tokens, "tertiary", secondary);
    const std::uint32_t primaryFixedDim = tokenOr(tokens, "primary_fixed_dim", primary);
    const std::uint32_t secondaryFixedDim = tokenOr(tokens, "secondary_fixed_dim", secondary);

    setMissingToken(tokens, "terminal_foreground", foreground);
    setMissingToken(tokens, "terminal_background", background);
    setMissingToken(tokens, "terminal_cursor", foreground);
    setMissingToken(tokens, "terminal_cursor_text", background);
    setMissingToken(tokens, "terminal_selection_fg", onSurfaceVariant);
    setMissingToken(tokens, "terminal_selection_bg", surfaceVariant);

    setMissingToken(tokens, "terminal_normal_black", surfaceVariant);
    setMissingToken(tokens, "terminal_normal_red", error);
    setMissingToken(tokens, "terminal_normal_green", primary);
    setMissingToken(tokens, "terminal_normal_yellow", secondary);
    setMissingToken(tokens, "terminal_normal_blue", tertiary);
    setMissingToken(tokens, "terminal_normal_magenta", primaryFixedDim);
    setMissingToken(tokens, "terminal_normal_cyan", secondaryFixedDim);
    setMissingToken(tokens, "terminal_normal_white", foreground);

    setMissingToken(tokens, "terminal_bright_black", outline);
    setMissingToken(tokens, "terminal_bright_red", error);
    setMissingToken(tokens, "terminal_bright_green", primary);
    setMissingToken(tokens, "terminal_bright_yellow", secondary);
    setMissingToken(tokens, "terminal_bright_blue", tertiary);
    setMissingToken(tokens, "terminal_bright_magenta", primaryFixedDim);
    setMissingToken(tokens, "terminal_bright_cyan", secondaryFixedDim);
    setMissingToken(tokens, "terminal_bright_white", foreground);
  }

  void synthesizeTerminalPaletteTokens(GeneratedPalette& palette) {
    if (!palette.dark.empty()) {
      synthesizeTerminalPaletteTokens(palette.dark);
    }
    if (!palette.light.empty()) {
      synthesizeTerminalPaletteTokens(palette.light);
    }
  }

  TokenMap expandFixedPaletteMode(const ::Palette& palette, bool isDark) {
    const Color primary = toUiColor(palette.primary);
    const Color onPrimary = toUiColor(palette.onPrimary);
    const Color secondary = toUiColor(palette.secondary);
    const Color onSecondary = toUiColor(palette.onSecondary);
    const Color tertiary = toUiColor(palette.tertiary);
    const Color onTertiary = toUiColor(palette.onTertiary);
    const Color error = toUiColor(palette.error);
    const Color onError = toUiColor(palette.onError);
    const Color surface = toUiColor(palette.surface);
    const Color onSurface = toUiColor(palette.onSurface);
    const Color surfaceVariant = toUiColor(palette.surfaceVariant);
    const Color onSurfaceVariant = toUiColor(palette.onSurfaceVariant);
    const Color outlineRaw = toUiColor(palette.outline);
    const Color shadow = toUiColor(palette.shadow);

    auto makeContainerDark = [](const Color& base) {
      auto [h, s, l] = base.toHsl();
      return Color::fromHsl(h, std::min(s + 0.15, 1.0), std::max(l - 0.35, 0.15));
    };
    auto makeContainerLight = [](const Color& base) {
      auto [h, s, l] = base.toHsl();
      return Color::fromHsl(h, std::max(s - 0.20, 0.30), std::min(l + 0.35, 0.85));
    };
    auto makeFixedDark = [](const Color& base) {
      auto [h, s, _] = base.toHsl();
      return std::make_tuple(Color::fromHsl(h, std::max(s, 0.70), 0.85), Color::fromHsl(h, std::max(s, 0.65), 0.75));
    };
    auto makeFixedLight = [](const Color& base) {
      auto [h, s, _] = base.toHsl();
      return std::make_tuple(Color::fromHsl(h, std::max(s, 0.70), 0.40), Color::fromHsl(h, std::max(s, 0.65), 0.30));
    };

    const Color primaryContainer = isDark ? makeContainerDark(primary) : makeContainerLight(primary);
    const Color secondaryContainer = isDark ? makeContainerDark(secondary) : makeContainerLight(secondary);
    const Color tertiaryContainer = isDark ? makeContainerDark(tertiary) : makeContainerLight(tertiary);
    const Color errorContainer = isDark ? makeContainerDark(error) : makeContainerLight(error);

    const auto [primaryH, primaryS, _primaryL] = primary.toHsl();
    const auto [secondaryH, secondaryS, _secondaryL] = secondary.toHsl();
    const auto [tertiaryH, tertiaryS, _tertiaryL] = tertiary.toHsl();
    const auto [errorH, errorS, _errorL] = error.toHsl();

    const Color onPrimaryContainer = isDark
        ? ensureContrast(Color::fromHsl(primaryH, primaryS, 0.90), primaryContainer, 4.5)
        : ensureContrast(Color::fromHsl(primaryH, primaryS, 0.15), primaryContainer, 4.5);
    const Color onSecondaryContainer = isDark
        ? ensureContrast(Color::fromHsl(secondaryH, secondaryS, 0.90), secondaryContainer, 4.5)
        : ensureContrast(Color::fromHsl(secondaryH, secondaryS, 0.15), secondaryContainer, 4.5);
    const Color onTertiaryContainer = isDark
        ? ensureContrast(Color::fromHsl(tertiaryH, tertiaryS, 0.90), tertiaryContainer, 4.5)
        : ensureContrast(Color::fromHsl(tertiaryH, tertiaryS, 0.15), tertiaryContainer, 4.5);
    const Color onErrorContainer = isDark ? ensureContrast(Color::fromHsl(errorH, errorS, 0.90), errorContainer, 4.5)
                                          : ensureContrast(Color::fromHsl(errorH, errorS, 0.15), errorContainer, 4.5);

    const auto [primaryFixed, primaryFixedDim] = isDark ? makeFixedDark(primary) : makeFixedLight(primary);
    const auto [secondaryFixed, secondaryFixedDim] = isDark ? makeFixedDark(secondary) : makeFixedLight(secondary);
    const auto [tertiaryFixed, tertiaryFixedDim] = isDark ? makeFixedDark(tertiary) : makeFixedLight(tertiary);

    const Color onPrimaryFixed = isDark ? ensureContrast(Color::fromHsl(primaryH, 0.15, 0.15), primaryFixed, 4.5)
                                        : ensureContrast(Color::fromHsl(primaryH, 0.15, 0.90), primaryFixed, 4.5);
    const Color onPrimaryFixedVariant = isDark
        ? ensureContrast(Color::fromHsl(primaryH, 0.15, 0.20), primaryFixedDim, 4.5)
        : ensureContrast(Color::fromHsl(primaryH, 0.15, 0.85), primaryFixedDim, 4.5);
    const Color onSecondaryFixed = isDark ? ensureContrast(Color::fromHsl(secondaryH, 0.15, 0.15), secondaryFixed, 4.5)
                                          : ensureContrast(Color::fromHsl(secondaryH, 0.15, 0.90), secondaryFixed, 4.5);
    const Color onSecondaryFixedVariant = isDark
        ? ensureContrast(Color::fromHsl(secondaryH, 0.15, 0.20), secondaryFixedDim, 4.5)
        : ensureContrast(Color::fromHsl(secondaryH, 0.15, 0.85), secondaryFixedDim, 4.5);
    const Color onTertiaryFixed = isDark ? ensureContrast(Color::fromHsl(tertiaryH, 0.15, 0.15), tertiaryFixed, 4.5)
                                         : ensureContrast(Color::fromHsl(tertiaryH, 0.15, 0.90), tertiaryFixed, 4.5);
    const Color onTertiaryFixedVariant = isDark
        ? ensureContrast(Color::fromHsl(tertiaryH, 0.15, 0.20), tertiaryFixedDim, 4.5)
        : ensureContrast(Color::fromHsl(tertiaryH, 0.15, 0.85), tertiaryFixedDim, 4.5);

    const auto [surfaceH, surfaceS, surfaceL] = surface.toHsl();
    const auto [surfaceVariantH, surfaceVariantS, surfaceVariantL] = surfaceVariant.toHsl();
    const Color surfaceContainer = surfaceVariant;
    const Color surfaceContainerLowest = interpolateColor(surface, surfaceVariant, 0.2);
    const Color surfaceContainerLow = interpolateColor(surface, surfaceVariant, 0.5);
    const Color surfaceContainerHigh = isDark
        ? Color::fromHsl(surfaceVariantH, surfaceVariantS, std::min(surfaceVariantL + 0.04, 0.40))
        : Color::fromHsl(surfaceVariantH, surfaceVariantS, std::max(surfaceVariantL - 0.04, 0.60));
    const Color surfaceContainerHighest = isDark
        ? Color::fromHsl(surfaceVariantH, surfaceVariantS, std::min(surfaceVariantL + 0.08, 0.45))
        : Color::fromHsl(surfaceVariantH, surfaceVariantS, std::max(surfaceVariantL - 0.08, 0.55));
    const Color surfaceDim = isDark
        ? Color::fromHsl(surfaceH, surfaceS, std::max(surfaceL - 0.04, 0.02))
        : Color::fromHsl(surfaceVariantH, surfaceVariantS, std::max(surfaceVariantL - 0.12, 0.50));
    const Color surfaceBright = isDark
        ? Color::fromHsl(surfaceVariantH, surfaceVariantS, std::min(surfaceVariantL + 0.12, 0.50))
        : Color::fromHsl(surfaceH, surfaceS, std::min(surfaceL + 0.03, 0.98));

    const Color outline = ensureContrast(outlineRaw, surface, 3.0);
    const auto [outlineH, outlineS, outlineL] = outline.toHsl();
    const Color outlineVariant = isDark ? Color::fromHsl(outlineH, outlineS, std::max(outlineL - 0.15, 0.1))
                                        : Color::fromHsl(outlineH, outlineS, std::min(outlineL + 0.15, 0.9));
    const Color scrim(0, 0, 0);
    const Color inverseSurface = isDark ? Color::fromHsl(surfaceH, 0.08, 0.90) : Color::fromHsl(surfaceH, 0.08, 0.15);
    const Color inverseOnSurface = isDark ? Color::fromHsl(surfaceH, 0.05, 0.15) : Color::fromHsl(surfaceH, 0.05, 0.90);
    const Color inversePrimary = isDark ? Color::fromHsl(primaryH, std::max(primaryS * 0.8, 0.5), 0.40)
                                        : Color::fromHsl(primaryH, std::max(primaryS * 0.8, 0.5), 0.70);
    const Color background = surface;
    const Color onBackground = onSurface;

    TokenMap result;
    setToken(result, "source_color", primary);
    setToken(result, "primary", primary);
    setToken(result, "on_primary", onPrimary);
    setToken(result, "primary_container", primaryContainer);
    setToken(result, "on_primary_container", onPrimaryContainer);
    setToken(result, "primary_fixed", primaryFixed);
    setToken(result, "primary_fixed_dim", primaryFixedDim);
    setToken(result, "on_primary_fixed", onPrimaryFixed);
    setToken(result, "on_primary_fixed_variant", onPrimaryFixedVariant);
    setToken(result, "secondary", secondary);
    setToken(result, "on_secondary", onSecondary);
    setToken(result, "secondary_container", secondaryContainer);
    setToken(result, "on_secondary_container", onSecondaryContainer);
    setToken(result, "secondary_fixed", secondaryFixed);
    setToken(result, "secondary_fixed_dim", secondaryFixedDim);
    setToken(result, "on_secondary_fixed", onSecondaryFixed);
    setToken(result, "on_secondary_fixed_variant", onSecondaryFixedVariant);
    setToken(result, "tertiary", tertiary);
    setToken(result, "on_tertiary", onTertiary);
    setToken(result, "tertiary_container", tertiaryContainer);
    setToken(result, "on_tertiary_container", onTertiaryContainer);
    setToken(result, "tertiary_fixed", tertiaryFixed);
    setToken(result, "tertiary_fixed_dim", tertiaryFixedDim);
    setToken(result, "on_tertiary_fixed", onTertiaryFixed);
    setToken(result, "on_tertiary_fixed_variant", onTertiaryFixedVariant);
    setToken(result, "error", error);
    setToken(result, "on_error", onError);
    setToken(result, "error_container", errorContainer);
    setToken(result, "on_error_container", onErrorContainer);
    setToken(result, "surface", surface);
    setToken(result, "on_surface", onSurface);
    setToken(result, "surface_variant", surfaceVariant);
    setToken(result, "on_surface_variant", onSurfaceVariant);
    setToken(result, "surface_dim", surfaceDim);
    setToken(result, "surface_bright", surfaceBright);
    setToken(result, "surface_container_lowest", surfaceContainerLowest);
    setToken(result, "surface_container_low", surfaceContainerLow);
    setToken(result, "surface_container", surfaceContainer);
    setToken(result, "surface_container_high", surfaceContainerHigh);
    setToken(result, "surface_container_highest", surfaceContainerHighest);
    setToken(result, "surface_tint", primary);
    setToken(result, "outline", outline);
    setToken(result, "outline_variant", outlineVariant);
    setToken(result, "shadow", shadow);
    setToken(result, "scrim", scrim);
    setToken(result, "inverse_surface", inverseSurface);
    setToken(result, "inverse_on_surface", inverseOnSurface);
    setToken(result, "inverse_primary", inversePrimary);
    setToken(result, "background", background);
    setToken(result, "on_background", onBackground);
    return result;
  }

  GeneratedPalette expandFixedPalettes(const ::Palette& dark, const ::Palette& light) {
    return GeneratedPalette{
        .dark = expandFixedPaletteMode(dark, true),
        .light = expandFixedPaletteMode(light, false),
    };
  }

  ::Palette mapGeneratedPaletteMode(const TokenMap& t) {
    return ::Palette{
        .primary = tokenToColor(t, "primary"),
        .onPrimary = tokenToColor(t, "on_primary"),
        .secondary = tokenToColor(t, "secondary"),
        .onSecondary = tokenToColor(t, "on_secondary"),
        .tertiary = tokenToColor(t, "tertiary"),
        .onTertiary = tokenToColor(t, "on_tertiary"),
        .error = tokenToColor(t, "error"),
        .onError = tokenToColor(t, "on_error"),
        .surface = tokenToColor(t, "surface"),
        .onSurface = tokenToColor(t, "on_surface"),
        .surfaceVariant = tokenToColor(t, "surface_container"),
        .onSurfaceVariant = tokenToColor(t, "on_surface_variant"),
        .outline = tokenToColor(t, "outline_variant"),
        .shadow = tokenToColor(t, "shadow"),
        .hover = tokenToColor(t, "tertiary"),
        .onHover = tokenToColor(t, "on_tertiary"),
    };
  }

} // namespace noctalia::theme
