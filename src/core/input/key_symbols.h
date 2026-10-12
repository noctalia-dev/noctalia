#pragma once

#include "core/input/key_modifiers.h"

#include <cstdint>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace KeySymbol {
  [[nodiscard]] inline bool isEnter(std::uint32_t sym) noexcept {
    return sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter;
  }

  [[nodiscard]] inline bool isEscape(std::uint32_t sym) noexcept { return sym == XKB_KEY_Escape; }

  [[nodiscard]] inline bool isSpace(std::uint32_t sym) noexcept { return sym == XKB_KEY_space; }

  [[nodiscard]] inline bool isEnterOrSpace(std::uint32_t sym) noexcept { return isEnter(sym) || isSpace(sym); }

  [[nodiscard]] inline bool isBackspace(std::uint32_t sym) noexcept { return sym == XKB_KEY_BackSpace; }

  [[nodiscard]] inline bool isDelete(std::uint32_t sym) noexcept { return sym == XKB_KEY_Delete; }

  [[nodiscard]] inline bool isBackspaceOrDelete(std::uint32_t sym) noexcept {
    return isBackspace(sym) || isDelete(sym);
  }

  [[nodiscard]] inline bool isInsert(std::uint32_t sym) noexcept { return sym == XKB_KEY_Insert; }

  [[nodiscard]] inline bool isLeft(std::uint32_t sym) noexcept { return sym == XKB_KEY_Left; }

  [[nodiscard]] inline bool isRight(std::uint32_t sym) noexcept { return sym == XKB_KEY_Right; }

  [[nodiscard]] inline bool isUp(std::uint32_t sym) noexcept { return sym == XKB_KEY_Up; }

  [[nodiscard]] inline bool isDown(std::uint32_t sym) noexcept { return sym == XKB_KEY_Down; }

  [[nodiscard]] inline bool isPageUp(std::uint32_t sym) noexcept {
    return sym == XKB_KEY_Page_Up || sym == XKB_KEY_KP_Page_Up;
  }

  [[nodiscard]] inline bool isPageDown(std::uint32_t sym) noexcept {
    return sym == XKB_KEY_Page_Down || sym == XKB_KEY_KP_Page_Down;
  }

  [[nodiscard]] inline bool isHome(std::uint32_t sym) noexcept { return sym == XKB_KEY_Home; }

  [[nodiscard]] inline bool isEnd(std::uint32_t sym) noexcept { return sym == XKB_KEY_End; }

  [[nodiscard]] inline bool isTab(std::uint32_t sym) noexcept {
    return sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab;
  }

  [[nodiscard]] inline bool isModifier(std::uint32_t sym) noexcept {
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
    case XKB_KEY_Caps_Lock:
    case XKB_KEY_Shift_Lock:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
    case XKB_KEY_Hyper_L:
    case XKB_KEY_Hyper_R:
    case XKB_KEY_ISO_Level3_Shift:
    case XKB_KEY_ISO_Level5_Shift:
    case XKB_KEY_Mode_switch:
    case XKB_KEY_Num_Lock:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] inline bool isSuperModifier(std::uint32_t sym) noexcept {
    return sym == XKB_KEY_Super_L
        || sym == XKB_KEY_Super_R
        || sym == XKB_KEY_Hyper_L
        || sym == XKB_KEY_Hyper_R
        || sym == XKB_KEY_Meta_L
        || sym == XKB_KEY_Meta_R;
  }

  [[nodiscard]] inline std::uint32_t modifierMask(std::uint32_t sym) noexcept {
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
      return KeyMod::Shift;
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
      return KeyMod::Ctrl;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
      return KeyMod::Alt;
    default:
      return isSuperModifier(sym) ? KeyMod::Super : 0;
    }
  }
} // namespace KeySymbol
