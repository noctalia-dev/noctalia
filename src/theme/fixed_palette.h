#pragma once

#include "theme/palette.h"
#include "ui/palette.h"

#include <array>
#include <string_view>
#include <unordered_map>

namespace noctalia::theme {

  using TokenMap = std::unordered_map<std::string, uint32_t>;

  inline constexpr std::string_view kTerminalJsonKey = "terminal";
  inline constexpr std::string_view kTerminalNormalJsonKey = "normal";
  inline constexpr std::string_view kTerminalBrightJsonKey = "bright";
  inline constexpr std::string_view kTerminalBlackJsonKey = "black";
  inline constexpr std::string_view kTerminalRedJsonKey = "red";
  inline constexpr std::string_view kTerminalGreenJsonKey = "green";
  inline constexpr std::string_view kTerminalYellowJsonKey = "yellow";
  inline constexpr std::string_view kTerminalBlueJsonKey = "blue";
  inline constexpr std::string_view kTerminalMagentaJsonKey = "magenta";
  inline constexpr std::string_view kTerminalCyanJsonKey = "cyan";
  inline constexpr std::string_view kTerminalWhiteJsonKey = "white";
  inline constexpr std::string_view kTerminalForegroundJsonKey = "foreground";
  inline constexpr std::string_view kTerminalBackgroundJsonKey = "background";
  inline constexpr std::string_view kTerminalCursorJsonKey = "cursor";
  inline constexpr std::string_view kTerminalCursorTextJsonKey = "cursor_text";
  inline constexpr std::string_view kTerminalSelectionFgJsonKey = "selection_fg";
  inline constexpr std::string_view kTerminalSelectionBgJsonKey = "selection_bg";

  struct TerminalColorTokenKey {
    std::string_view jsonKey;
    std::string_view tokenKey;
  };

  inline constexpr std::array<TerminalColorTokenKey, 6> kTerminalDirectColorTokenKeys = {{
      {kTerminalForegroundJsonKey, "terminal_foreground"},
      {kTerminalBackgroundJsonKey, "terminal_background"},
      {kTerminalCursorJsonKey, "terminal_cursor"},
      {kTerminalCursorTextJsonKey, "terminal_cursor_text"},
      {kTerminalSelectionFgJsonKey, "terminal_selection_fg"},
      {kTerminalSelectionBgJsonKey, "terminal_selection_bg"},
  }};

  inline constexpr std::array<std::string_view, 8> kTerminalAnsiColorJsonKeys = {{
      kTerminalBlackJsonKey,
      kTerminalRedJsonKey,
      kTerminalGreenJsonKey,
      kTerminalYellowJsonKey,
      kTerminalBlueJsonKey,
      kTerminalMagentaJsonKey,
      kTerminalCyanJsonKey,
      kTerminalWhiteJsonKey,
  }};

  struct TerminalAnsiGroupTokenKey {
    std::string_view jsonKey;
    std::string_view tokenPrefix;
  };

  inline constexpr std::array<TerminalAnsiGroupTokenKey, 2> kTerminalAnsiGroupTokenKeys = {{
      {kTerminalNormalJsonKey, "terminal_normal"},
      {kTerminalBrightJsonKey, "terminal_bright"},
  }};

  struct TerminalAnsiColors {
    ::Color black;
    ::Color red;
    ::Color green;
    ::Color yellow;
    ::Color blue;
    ::Color magenta;
    ::Color cyan;
    ::Color white;
  };

  struct TerminalPalette {
    TerminalAnsiColors normal;
    TerminalAnsiColors bright;
    ::Color foreground;
    ::Color background;
    ::Color selectionFg;
    ::Color selectionBg;
    ::Color cursorText;
    ::Color cursor;
  };

  // Source format of builtin, community, and custom palettes: the 16 hand-picked colors that
  // expandFixedPaletteMode() grows into the full token set. Not the runtime UI palette.
  struct FixedPalette {
    ::Color primary;
    ::Color onPrimary;
    ::Color secondary;
    ::Color onSecondary;
    ::Color tertiary;
    ::Color onTertiary;
    ::Color error;
    ::Color onError;
    ::Color surface;
    ::Color onSurface;
    ::Color surfaceContainer;
    ::Color onSurfaceVariant;
    ::Color outlineVariant;
    ::Color shadow;
    ::Color hover;
    ::Color onHover;
  };

  struct FixedPaletteJsonKey {
    std::string_view jsonKey;
    ::Color FixedPalette::* field;
  };

  // Palette JSON keys, in file order. Each is the name of the color role it feeds.
  inline constexpr std::array<FixedPaletteJsonKey, 16> kFixedPaletteJsonKeys = {{
      {"primary", &FixedPalette::primary},
      {"on_primary", &FixedPalette::onPrimary},
      {"secondary", &FixedPalette::secondary},
      {"on_secondary", &FixedPalette::onSecondary},
      {"tertiary", &FixedPalette::tertiary},
      {"on_tertiary", &FixedPalette::onTertiary},
      {"error", &FixedPalette::error},
      {"on_error", &FixedPalette::onError},
      {"surface", &FixedPalette::surface},
      {"on_surface", &FixedPalette::onSurface},
      {"surface_container", &FixedPalette::surfaceContainer},
      {"on_surface_variant", &FixedPalette::onSurfaceVariant},
      {"outline_variant", &FixedPalette::outlineVariant},
      {"shadow", &FixedPalette::shadow},
      {"hover", &FixedPalette::hover},
      {"on_hover", &FixedPalette::onHover},
  }};

  TokenMap expandFixedPaletteMode(const FixedPalette& palette, bool isDark);
  GeneratedPalette expandFixedPalettes(const FixedPalette& dark, const FixedPalette& light);
  // Inverse of expandFixedPaletteMode(): reads the source colors back out of a token set.
  [[nodiscard]] FixedPalette fixedPaletteFromTokens(const TokenMap& tokens);
  ::Palette mapGeneratedPaletteMode(const TokenMap& tokens);

  // Wallpaper schemes have no hover role of their own: hover mirrors the tertiary accent,
  // the pairing the builtin palettes use.
  void setGeneratedHoverTokens(GeneratedPalette& palette);

  void applyTerminalPalette(TokenMap& tokens, const TerminalPalette& terminal);
  void synthesizeTerminalPaletteTokens(TokenMap& tokens);
  void synthesizeTerminalPaletteTokens(GeneratedPalette& palette);
  [[nodiscard]] TerminalPalette terminalPaletteFromTokens(const TokenMap& tokens);

} // namespace noctalia::theme
