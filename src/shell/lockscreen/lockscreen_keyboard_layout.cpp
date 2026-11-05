#include "shell/lockscreen/lockscreen_keyboard_layout.h"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace lockscreen_keyboard {

  namespace {

    constexpr KeyCap ch(std::string_view text) { return KeyCap{.action = KeyAction::Character, .text = text}; }
    constexpr KeyCap special(KeyAction action, float width) { return KeyCap{.action = action, .width = width}; }

    // Shared bottom row: [hide] [layer] [space] [.] [enter]
    constexpr std::array kBottomRow = {
        special(KeyAction::Hide, 1.0F),  special(KeyAction::LayerToggle, 1.5F),
        special(KeyAction::Space, 5.0F), ch("."),
        special(KeyAction::Enter, 1.5F),
    };

    constexpr std::array kLowerRow0 = {ch("q"), ch("w"), ch("e"), ch("r"), ch("t"),
                                       ch("y"), ch("u"), ch("i"), ch("o"), ch("p")};
    constexpr std::array kLowerRow1 = {ch("a"), ch("s"), ch("d"), ch("f"), ch("g"), ch("h"), ch("j"), ch("k"), ch("l")};
    constexpr std::array kLowerRow2 = {
        special(KeyAction::Shift, 1.5F),     ch("z"), ch("x"), ch("c"), ch("v"), ch("b"), ch("n"), ch("m"),
        special(KeyAction::Backspace, 1.5F),
    };

    constexpr std::array kUpperRow0 = {ch("Q"), ch("W"), ch("E"), ch("R"), ch("T"),
                                       ch("Y"), ch("U"), ch("I"), ch("O"), ch("P")};
    constexpr std::array kUpperRow1 = {ch("A"), ch("S"), ch("D"), ch("F"), ch("G"), ch("H"), ch("J"), ch("K"), ch("L")};
    constexpr std::array kUpperRow2 = {
        special(KeyAction::Shift, 1.5F),     ch("Z"), ch("X"), ch("C"), ch("V"), ch("B"), ch("N"), ch("M"),
        special(KeyAction::Backspace, 1.5F),
    };

    constexpr std::array kSymbolsRow0 = {ch("1"), ch("2"), ch("3"), ch("4"), ch("5"),
                                         ch("6"), ch("7"), ch("8"), ch("9"), ch("0")};
    constexpr std::array kSymbolsRow1 = {ch("@"), ch("#"), ch("$"), ch("%"), ch("&"),
                                         ch("-"), ch("+"), ch("("), ch(")")};
    constexpr std::array kSymbolsRow2 = {
        special(KeyAction::Shift, 1.5F),     ch("*"), ch("\""), ch("'"), ch(":"), ch(";"), ch("!"), ch("?"),
        special(KeyAction::Backspace, 1.5F),
    };

    constexpr std::array kSymbolsAltRow0 = {ch("~"), ch("`"), ch("|"), ch("\\"), ch("/"),
                                            ch("_"), ch("="), ch("^"), ch("["),  ch("]")};
    constexpr std::array kSymbolsAltRow1 = {ch("{"), ch("}"), ch("<"), ch(">"), ch(","),
                                            ch("€"), ch("£"), ch("¥"), ch("°")};
    constexpr std::array kSymbolsAltRow2 = {
        special(KeyAction::Shift, 1.5F),     ch("§"), ch("¢"), ch("×"), ch("÷"), ch("¿"), ch("¡"), ch("•"),
        special(KeyAction::Backspace, 1.5F),
    };

    const LayerRows kLowerRows = {kLowerRow0, kLowerRow1, kLowerRow2, kBottomRow};
    const LayerRows kUpperRows = {kUpperRow0, kUpperRow1, kUpperRow2, kBottomRow};
    const LayerRows kSymbolsRows = {kSymbolsRow0, kSymbolsRow1, kSymbolsRow2, kBottomRow};
    const LayerRows kSymbolsAltRows = {kSymbolsAltRow0, kSymbolsAltRow1, kSymbolsAltRow2, kBottomRow};

    // Decodes the first UTF-8 codepoint of `text`; 0 for empty or malformed input.
    std::uint32_t firstCodepoint(std::string_view text) noexcept {
      if (text.empty()) {
        return 0;
      }
      const auto b0 = static_cast<unsigned char>(text[0]);
      std::size_t length = 0;
      std::uint32_t cp = 0;
      if (b0 < 0x80U) {
        return b0;
      }
      if ((b0 & 0xE0U) == 0xC0U) {
        length = 2;
        cp = b0 & 0x1FU;
      } else if ((b0 & 0xF0U) == 0xE0U) {
        length = 3;
        cp = b0 & 0x0FU;
      } else if ((b0 & 0xF8U) == 0xF0U) {
        length = 4;
        cp = b0 & 0x07U;
      } else {
        return 0;
      }
      if (text.size() < length) {
        return 0;
      }
      for (std::size_t i = 1; i < length; ++i) {
        const auto b = static_cast<unsigned char>(text[i]);
        if ((b & 0xC0U) != 0x80U) {
          return 0;
        }
        cp = (cp << 6U) | (b & 0x3FU);
      }
      return cp;
    }

    KeyOutput fromKeysym(std::uint32_t sym) noexcept {
      // Match what a hardware key reports: xkb gives Return -> '\r' and BackSpace -> '\b'.
      return KeyOutput{.sym = sym, .utf32 = xkb_keysym_to_utf32(sym)};
    }

  } // namespace

  const LayerRows& rowsFor(Layer layer) noexcept {
    switch (layer) {
    case Layer::Lower:
      return kLowerRows;
    case Layer::Upper:
      return kUpperRows;
    case Layer::Symbols:
      return kSymbolsRows;
    case Layer::SymbolsAlt:
      return kSymbolsAltRows;
    }
    return kLowerRows;
  }

  float rowInset(std::size_t row) noexcept { return row == 1 ? 0.5F : 0.0F; }

  std::optional<KeyOutput> outputFor(const KeyCap& key) noexcept {
    switch (key.action) {
    case KeyAction::Character: {
      const std::uint32_t cp = firstCodepoint(key.text);
      if (cp == 0) {
        return std::nullopt;
      }
      const xkb_keysym_t sym = xkb_utf32_to_keysym(cp);
      if (sym == XKB_KEY_NoSymbol) {
        return std::nullopt;
      }
      return KeyOutput{.sym = sym, .utf32 = cp};
    }
    case KeyAction::Backspace:
      return fromKeysym(XKB_KEY_BackSpace);
    case KeyAction::Enter:
      return fromKeysym(XKB_KEY_Return);
    case KeyAction::Space:
      return fromKeysym(XKB_KEY_space);
    case KeyAction::Shift:
    case KeyAction::LayerToggle:
    case KeyAction::Hide:
      return std::nullopt;
    }
    return std::nullopt;
  }

  Layer nextLayer(Layer current, KeyAction action) noexcept {
    switch (action) {
    case KeyAction::Shift:
      switch (current) {
      case Layer::Lower:
        return Layer::Upper;
      case Layer::Upper:
        return Layer::Lower;
      case Layer::Symbols:
        return Layer::SymbolsAlt;
      case Layer::SymbolsAlt:
        return Layer::Symbols;
      }
      return current;
    case KeyAction::LayerToggle:
      return (current == Layer::Lower || current == Layer::Upper) ? Layer::Symbols : Layer::Lower;
    case KeyAction::Character:
      return current == Layer::Upper ? Layer::Lower : current;
    case KeyAction::Backspace:
    case KeyAction::Enter:
    case KeyAction::Space:
    case KeyAction::Hide:
      return current;
    }
    return current;
  }

  std::string_view specialLabel(KeyAction action, Layer layer) noexcept {
    switch (action) {
    case KeyAction::LayerToggle:
      return (layer == Layer::Lower || layer == Layer::Upper) ? "?123" : "ABC";
    case KeyAction::Shift:
      if (layer == Layer::Symbols) {
        return "=\\<";
      }
      if (layer == Layer::SymbolsAlt) {
        return "?123";
      }
      return {};
    default:
      return {};
    }
  }

  std::string_view specialGlyph(KeyAction action, Layer layer) noexcept {
    switch (action) {
    case KeyAction::Shift:
      if (layer == Layer::Lower) {
        return "arrow-big-up";
      }
      if (layer == Layer::Upper) {
        return "arrow-big-up-filled";
      }
      return {};
    case KeyAction::Backspace:
      return "backspace";
    case KeyAction::Enter:
      return "corner-down-left";
    case KeyAction::Space:
      return "space";
    case KeyAction::Hide:
      return "keyboard-hide";
    default:
      return {};
    }
  }

} // namespace lockscreen_keyboard
