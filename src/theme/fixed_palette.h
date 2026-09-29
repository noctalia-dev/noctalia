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
  inline constexpr std::string_view kTerminalCursorTextJsonKey = "cursorText";
  inline constexpr std::string_view kTerminalSelectionFgJsonKey = "selectionFg";
  inline constexpr std::string_view kTerminalSelectionBgJsonKey = "selectionBg";

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

  TokenMap expandFixedPaletteMode(const ::Palette& palette, bool isDark);
  GeneratedPalette expandFixedPalettes(const ::Palette& dark, const ::Palette& light);
  ::Palette mapGeneratedPaletteMode(const TokenMap& tokens);

  // Wallpaper schemes have no hover role of their own: hover mirrors the tertiary accent,
  // the pairing the builtin palettes use.
  void setGeneratedHoverTokens(GeneratedPalette& palette);

  void applyTerminalPalette(TokenMap& tokens, const TerminalPalette& terminal);
  void synthesizeTerminalPaletteTokens(TokenMap& tokens);
  void synthesizeTerminalPaletteTokens(GeneratedPalette& palette);
  [[nodiscard]] TerminalPalette terminalPaletteFromTokens(const TokenMap& tokens);

} // namespace noctalia::theme
