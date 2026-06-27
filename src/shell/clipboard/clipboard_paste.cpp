#include "shell/clipboard/clipboard_paste.h"

#include "config/config_types.h"
#include "core/log.h"
#include "core/process.h"
#include "wayland/virtual_keyboard_service.h"

#include <optional>
#include <vector>

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

    std::vector<std::string> wtypeArgsFor(ClipboardAutoPasteMode mode, bool isImage) {
      switch (mode) {
      case ClipboardAutoPasteMode::Off:
        return {};
      case ClipboardAutoPasteMode::Auto:
        return isImage
            ? std::vector<std::string>{"wtype", "-M", "ctrl", "-k", "v", "-m", "ctrl"}
            : std::vector<std::string>{"wtype", "-M", "ctrl", "-M", "shift", "-k", "v", "-m", "shift", "-m", "ctrl"};
      case ClipboardAutoPasteMode::CtrlV:
        return {"wtype", "-M", "ctrl", "-k", "v", "-m", "ctrl"};
      case ClipboardAutoPasteMode::CtrlShiftV:
        return {"wtype", "-M", "ctrl", "-M", "shift", "-k", "v", "-m", "shift", "-m", "ctrl"};
      case ClipboardAutoPasteMode::ShiftInsert:
        return {"wtype", "-M", "shift", "-k", "Insert", "-m", "shift"};
      }
      return {};
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

    const auto args = wtypeArgsFor(mode, isImage);
    if (!args.empty() && process::runAsync(args)) {
      return true;
    }

    kLog.warn("clipboard auto-paste failed: native virtual keyboard unavailable and wtype launch failed");
    return false;
  }

} // namespace clipboard_paste
