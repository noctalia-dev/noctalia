#include "wayland/virtual_keyboard_service.h"

#include "core/log.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon.h>

namespace {

  constexpr Logger kLog("virtual-keyboard");

  std::uint32_t modifierMask(xkb_keymap* keymap, const char* name) {
    if (keymap == nullptr || name == nullptr) {
      return 0;
    }
    const xkb_mod_index_t index = xkb_keymap_mod_get_index(keymap, name);
    if (index == XKB_MOD_INVALID || index >= 32) {
      return 0;
    }
    return 1u << index;
  }

  std::uint32_t eventTimeMs() {
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
  }

} // namespace

VirtualKeyboardService::VirtualKeyboardService() = default;

VirtualKeyboardService::~VirtualKeyboardService() { cleanup(); }

bool VirtualKeyboardService::bind(zwp_virtual_keyboard_manager_v1* manager, wl_seat* seat) {
  if (manager == nullptr || seat == nullptr) {
    cleanup();
    return false;
  }

  if (m_manager == manager && m_seat == seat && m_keyboard != nullptr) {
    return true;
  }

  cleanup();
  m_manager = manager;
  m_seat = seat;
  return ensureKeyboard();
}

void VirtualKeyboardService::cleanup() {
  if (m_keyboard != nullptr) {
    zwp_virtual_keyboard_v1_destroy(m_keyboard);
    m_keyboard = nullptr;
  }
  if (m_xkbKeymap != nullptr) {
    xkb_keymap_unref(m_xkbKeymap);
    m_xkbKeymap = nullptr;
  }
  if (m_xkbContext != nullptr) {
    xkb_context_unref(m_xkbContext);
    m_xkbContext = nullptr;
  }
  m_manager = nullptr;
  m_seat = nullptr;
  m_ctrlMask = 0;
  m_shiftMask = 0;
  m_keymapUploaded = false;
}

bool VirtualKeyboardService::isAvailable() const noexcept { return m_manager != nullptr && m_seat != nullptr; }

bool VirtualKeyboardService::sendPasteShortcut(VirtualPasteShortcut shortcut) {
  if (!ensureKeyboard() || !ensureKeymap()) {
    return false;
  }

  switch (shortcut) {
  case VirtualPasteShortcut::CtrlV:
    pressChord(KEY_V, true, false);
    break;
  case VirtualPasteShortcut::CtrlShiftV:
    pressChord(KEY_V, true, true);
    break;
  case VirtualPasteShortcut::ShiftInsert:
    pressChord(KEY_INSERT, false, true);
    break;
  }

  auto* display = wl_proxy_get_display(reinterpret_cast<wl_proxy*>(m_keyboard));
  if (display != nullptr) {
    (void)wl_display_flush(display);
  }
  return true;
}

bool VirtualKeyboardService::ensureKeyboard() {
  if (m_keyboard != nullptr) {
    return true;
  }
  if (m_manager == nullptr || m_seat == nullptr) {
    return false;
  }

  m_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(m_manager, m_seat);
  if (m_keyboard == nullptr) {
    kLog.warn("failed to create virtual keyboard");
    return false;
  }
  return true;
}

bool VirtualKeyboardService::ensureKeymap() {
  if (m_keymapUploaded) {
    return true;
  }
  if (m_keyboard == nullptr) {
    return false;
  }
  if (m_xkbContext == nullptr) {
    m_xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  }
  if (m_xkbContext == nullptr) {
    kLog.warn("failed to create xkb context");
    return false;
  }
  if (m_xkbKeymap == nullptr) {
    m_xkbKeymap = xkb_keymap_new_from_names(m_xkbContext, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
  }
  if (m_xkbKeymap == nullptr) {
    kLog.warn("failed to compile virtual keyboard keymap");
    return false;
  }

  char* keymapString = xkb_keymap_get_as_string(m_xkbKeymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  if (keymapString == nullptr) {
    kLog.warn("failed to serialize virtual keyboard keymap");
    return false;
  }

  char path[] = "/tmp/noctalia-virtual-keyboard-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) {
    std::free(keymapString);
    kLog.warn("failed to allocate temporary keymap file");
    return false;
  }
  unlink(path);

  const std::size_t size = std::strlen(keymapString) + 1;
  const bool wroteAll = write(fd, keymapString, size) == static_cast<ssize_t>(size);
  std::free(keymapString);
  if (!wroteAll) {
    close(fd);
    kLog.warn("failed to write virtual keyboard keymap");
    return false;
  }

  zwp_virtual_keyboard_v1_keymap(m_keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<std::uint32_t>(size));
  close(fd);

  m_ctrlMask = modifierMask(m_xkbKeymap, XKB_MOD_NAME_CTRL);
  m_shiftMask = modifierMask(m_xkbKeymap, XKB_MOD_NAME_SHIFT);
  m_keymapUploaded = true;
  return true;
}

void VirtualKeyboardService::pressChord(std::uint32_t key, bool ctrlPressed, bool shiftPressed) {
  if (ctrlPressed) {
    sendKey(KEY_LEFTCTRL, true);
  }
  if (shiftPressed) {
    sendKey(KEY_LEFTSHIFT, true);
  }
  updateModifiers(ctrlPressed, shiftPressed);
  sendKey(key, true);
  sendKey(key, false);
  if (shiftPressed) {
    sendKey(KEY_LEFTSHIFT, false);
  }
  if (ctrlPressed) {
    sendKey(KEY_LEFTCTRL, false);
  }
  updateModifiers(false, false);
}

void VirtualKeyboardService::sendKey(std::uint32_t key, bool pressed) {
  if (m_keyboard == nullptr) {
    return;
  }
  zwp_virtual_keyboard_v1_key(
      m_keyboard, eventTimeMs(), key, pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED
  );
}

void VirtualKeyboardService::updateModifiers(bool ctrlPressed, bool shiftPressed) {
  if (m_keyboard == nullptr) {
    return;
  }
  std::uint32_t depressed = 0;
  if (ctrlPressed) {
    depressed |= m_ctrlMask;
  }
  if (shiftPressed) {
    depressed |= m_shiftMask;
  }
  zwp_virtual_keyboard_v1_modifiers(m_keyboard, depressed, 0, 0, 0);
}
