#include "core/input/key_chord.h"

#include "core/input/key_modifiers.h"
#include "util/string_utils.h"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace {
  std::string canonicalKeyName(std::string raw) {
    const std::string lower = StringUtils::toLower(raw);
    if (lower == "esc") {
      return "Escape";
    }
    if (lower == "del") {
      return "Delete";
    }
    if (lower == "enter") {
      return "Return";
    }
    if (lower == "kp_enter") {
      return "KP_Enter";
    }
    if (lower == "space" || lower == "spacebar") {
      return "space";
    }
    if (lower == "left") {
      return "Left";
    }
    if (lower == "right") {
      return "Right";
    }
    if (lower == "up") {
      return "Up";
    }
    if (lower == "down") {
      return "Down";
    }
    if (lower == "tab") {
      return "Tab";
    }
    if (lower == "iso_left_tab" || lower == "shift_tab") {
      return "ISO_Left_Tab";
    }
    return raw;
  }

  std::string keysymName(std::uint32_t sym) {
    if (sym == 0) {
      return {};
    }
    std::array<char, 64> buf{};
    const int n = xkb_keysym_get_name(static_cast<xkb_keysym_t>(sym), buf.data(), buf.size());
    if (n <= 0) {
      return {};
    }
    return std::string(buf.data(), static_cast<std::size_t>(n));
  }
} // namespace

std::optional<KeyChord> parseKeyChordSpec(std::string_view rawSpec) {
  const std::string spec = StringUtils::trim(rawSpec);
  if (spec.empty()) {
    return std::nullopt;
  }

  std::vector<std::string> tokens;
  std::size_t start = 0;
  while (start <= spec.size()) {
    const std::size_t plus = spec.find('+', start);
    const std::size_t len = (plus == std::string::npos) ? (spec.size() - start) : (plus - start);
    const std::string token = StringUtils::trim(std::string_view(spec).substr(start, len));
    if (token.empty()) {
      return std::nullopt;
    }
    tokens.push_back(token);
    if (plus == std::string::npos) {
      break;
    }
    start = plus + 1;
  }

  if (tokens.empty()) {
    return std::nullopt;
  }

  std::uint32_t modifiers = 0;
  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    const std::string mod = StringUtils::toLower(tokens[i]);
    if (mod == "ctrl" || mod == "control" || mod == "ctl") {
      modifiers |= KeyMod::Ctrl;
    } else if (mod == "shift") {
      modifiers |= KeyMod::Shift;
    } else if (mod == "alt" || mod == "option") {
      modifiers |= KeyMod::Alt;
    } else if (mod == "super" || mod == "meta" || mod == "logo" || mod == "win" || mod == "mod4") {
      throw std::runtime_error("modifier \"super/windows\" is not allowed");
    } else {
      return std::nullopt;
    }
  }

  const std::string keyName = canonicalKeyName(tokens.back());
  const xkb_keysym_t sym = xkb_keysym_from_name(keyName.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
  if (sym == XKB_KEY_NoSymbol) {
    return std::nullopt;
  }

  return KeyChord{.sym = static_cast<std::uint32_t>(sym), .modifiers = modifiers};
}

std::string keyChordToString(const KeyChord& chord) {
  const std::string keyName = keysymName(chord.sym);
  if (keyName.empty()) {
    return {};
  }
  std::string out;
  if ((chord.modifiers & KeyMod::Ctrl) != 0) {
    out += "Ctrl+";
  }
  if ((chord.modifiers & KeyMod::Alt) != 0) {
    out += "Alt+";
  }
  if ((chord.modifiers & KeyMod::Shift) != 0) {
    out += "Shift+";
  }
  out += keyName;
  return out;
}

std::string keyChordDisplayLabel(const KeyChord& chord) {
  if (chord.sym == 0) {
    return {};
  }

  std::string keyName;

  // Prefer the printable glyph over the raw XKB name (e.g. "udiaeresis" -> "Ü").
  const std::uint32_t cp = xkb_keysym_to_utf32(static_cast<xkb_keysym_t>(chord.sym));
  if (cp > 0x20 && cp != 0x7F) {
    std::array<char, 8> utf8Buf{};
    const int n = xkb_keysym_to_utf8(static_cast<xkb_keysym_t>(chord.sym), utf8Buf.data(), utf8Buf.size());
    if (n > 1) {
      keyName.assign(utf8Buf.data(), static_cast<std::size_t>(n - 1));
      if (keyName.size() == 1 && keyName[0] >= 'a' && keyName[0] <= 'z') {
        keyName[0] = static_cast<char>(keyName[0] - 0x20);
      }
    }
  }

  if (keyName.empty()) {
    keyName = keysymName(chord.sym);
    if (keyName.empty()) {
      return {};
    }
    if (keyName == "Return") {
      keyName = "Enter";
    } else if (keyName == "KP_Enter") {
      keyName = "Numpad Enter";
    } else if (keyName == "space") {
      keyName = "Space";
    } else if (keyName == "Prior") {
      keyName = "Page Up";
    } else if (keyName == "Next") {
      keyName = "Page Down";
    }
  }

  std::string out;
  const auto appendPart = [&](std::string_view part) {
    if (!out.empty()) {
      out += " + ";
    }
    out += part;
  };
  if ((chord.modifiers & KeyMod::Ctrl) != 0) {
    appendPart("Ctrl");
  }
  if ((chord.modifiers & KeyMod::Alt) != 0) {
    appendPart("Alt");
  }
  if ((chord.modifiers & KeyMod::Shift) != 0) {
    appendPart("Shift");
  }
  appendPart(keyName);
  return out;
}

bool keyChordMatches(const KeyChord& chord, std::uint32_t sym, std::uint32_t modifiers) noexcept {
  return chord.sym == sym && chord.modifiers == modifiers;
}

bool isPrintableKey(std::uint32_t sym) {
  if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
    return true;
  }
  if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {
    return true;
  }
  if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
    return true;
  }
  switch (sym) {
  case XKB_KEY_space:
  case XKB_KEY_exclam:
  case XKB_KEY_quotedbl:
  case XKB_KEY_numbersign:
  case XKB_KEY_dollar:
  case XKB_KEY_percent:
  case XKB_KEY_ampersand:
  case XKB_KEY_apostrophe:
  case XKB_KEY_parenleft:
  case XKB_KEY_parenright:
  case XKB_KEY_asterisk:
  case XKB_KEY_plus:
  case XKB_KEY_comma:
  case XKB_KEY_minus:
  case XKB_KEY_period:
  case XKB_KEY_slash:
  case XKB_KEY_colon:
  case XKB_KEY_semicolon:
  case XKB_KEY_less:
  case XKB_KEY_equal:
  case XKB_KEY_greater:
  case XKB_KEY_question:
  case XKB_KEY_at:
  case XKB_KEY_bracketleft:
  case XKB_KEY_backslash:
  case XKB_KEY_bracketright:
  case XKB_KEY_asciicircum:
  case XKB_KEY_underscore:
  case XKB_KEY_grave:
  case XKB_KEY_braceleft:
  case XKB_KEY_bar:
  case XKB_KEY_braceright:
  case XKB_KEY_asciitilde:
    return true;
  default:
    return false;
  }
}

bool isPlainPrintableKey(std::uint32_t utf32, std::uint32_t modifiers, bool preedit) noexcept {
  return !preedit
      && utf32 >= 0x20U
      && utf32 != 0x7FU
      && (modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0;
}
