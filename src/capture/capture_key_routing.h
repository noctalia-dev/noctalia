#pragma once

#include "config/config_types.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"

#include <cstdint>

namespace capture {

  /// What a key does while a region capture owns the keyboard.
  enum class CaptureKeyAction : std::uint8_t {
    Pass,    ///< Not a press: let it fall through as before.
    Cancel,  ///< The cancel keybind: drop the capture.
    Confirm, ///< Enter or Space on a pending selection: confirm with the defaults.
    Copy,    ///< The copy keybind on a pending selection: force the clipboard.
    Save,    ///< The save keybind on a pending selection: force a file.
    Swallow, ///< Any other press: consumed so it never reaches a panel.
  };

  /// Routes a key for an active region capture. A compositor without a focus grab (niri) keeps the
  /// keyboard on an older exclusive layer surface, such as the launcher, while the overlay is up. The
  /// capture therefore decides every press itself, whichever Noctalia surface holds the keyboard: the
  /// confirm and cancel keys act, and everything else is swallowed so no key lands in a panel.
  [[nodiscard]] inline CaptureKeyAction
  regionCaptureKeyAction(bool pressed, std::uint32_t sym, std::uint32_t modifiers, bool confirming) noexcept {
    if (!pressed) {
      return CaptureKeyAction::Pass;
    }
    if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
      return CaptureKeyAction::Cancel;
    }
    if (confirming) {
      if (KeybindMatcher::matches(KeybindAction::Copy, sym, modifiers)) {
        return CaptureKeyAction::Copy;
      }
      if (KeybindMatcher::matches(KeybindAction::Save, sym, modifiers)) {
        return CaptureKeyAction::Save;
      }
      if (KeySymbol::isEnterOrSpace(sym)) {
        return CaptureKeyAction::Confirm;
      }
    }
    return CaptureKeyAction::Swallow;
  }

} // namespace capture
