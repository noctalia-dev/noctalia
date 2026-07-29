#include "shell/clipboard/clipboard_paste.h"

#include "config/config_types.h"
#include "core/log.h"
#include "wayland/virtual_keyboard_service.h"

#include <optional>

namespace clipboard_paste {

  namespace {

    constexpr Logger kLog("clipboard");

    std::optional<VirtualPasteShortcut> virtualPasteShortcutFor(ClipboardAutoPasteMode mode, bool isImage) {
      switch (mode) {
      case ClipboardAutoPasteMode::Off:
        return std::nullopt;
      case ClipboardAutoPasteMode::Auto:
        return isImage ? VirtualPasteShortcut::CtrlV : VirtualPasteShortcut::CtrlShiftV;
      case ClipboardAutoPasteMode::CtrlV:
        return VirtualPasteShortcut::CtrlV;
      case ClipboardAutoPasteMode::CtrlShiftV:
        return VirtualPasteShortcut::CtrlShiftV;
      case ClipboardAutoPasteMode::ShiftInsert:
        return VirtualPasteShortcut::ShiftInsert;
      }
      return std::nullopt;
    }

  } // namespace

  bool pasteEntry(bool isImage, ClipboardAutoPasteMode mode, VirtualKeyboardService& virtualKeyboard) {
    if (mode == ClipboardAutoPasteMode::Off) {
      return true;
    }
    const auto shortcut = virtualPasteShortcutFor(mode, isImage);
    if (shortcut.has_value() && virtualKeyboard.sendPasteShortcut(*shortcut)) {
      return true;
    }

    kLog.warn("clipboard auto-paste failed: Wayland virtual keyboard unavailable");
    return false;
  }

} // namespace clipboard_paste
