#include "shell/wayland/clipboard_globals.h"

#include "ext-data-control-v1-client-protocol.h"
#include "shell/wayland/clipboard_service.h"
#include "shell/wayland/virtual_keyboard_service.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wayland/wayland_connection.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

#include <algorithm>

namespace {

  constexpr std::uint32_t kVirtualKeyboardManagerVersion = 1;

} // namespace

ClipboardGlobals::ClipboardGlobals(WaylandConnection& connection) : m_connection(connection) {}

void ClipboardGlobals::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == wl_seat_interface.name) {
    bindClipboardService();
    return;
  }

  if (interface == ext_data_control_manager_v1_interface.name) {
    if (m_dataControlManager != nullptr && m_dataControlOps != extDataControlOps()) {
      m_dataControlOps->destroyManager(m_dataControlManager);
      m_dataControlManager = nullptr;
    }
    if (m_dataControlManager == nullptr) {
      m_dataControlManager = extDataControlOps()->bindManager(registry, name, version);
      m_dataControlOps = extDataControlOps();
      bindClipboardService();
    }
    return;
  }

  if (interface == zwlr_data_control_manager_v1_interface.name) {
    if (m_dataControlManager == nullptr) {
      m_dataControlManager = wlrDataControlOps()->bindManager(registry, name, version);
      m_dataControlOps = wlrDataControlOps();
      bindClipboardService();
    }
    return;
  }

  if (interface == zwp_virtual_keyboard_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kVirtualKeyboardManagerVersion);
    m_virtualKeyboardManager = static_cast<zwp_virtual_keyboard_manager_v1*>(
        wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, bindVersion)
    );
    bindVirtualKeyboardService();
  }
}

void ClipboardGlobals::releaseGlobals() {
  if (m_clipboardService != nullptr) {
    m_clipboardService->cleanup();
  }
  if (m_virtualKeyboardService != nullptr) {
    m_virtualKeyboardService->cleanup();
  }
  if (m_dataControlManager != nullptr && m_dataControlOps != nullptr) {
    m_dataControlOps->destroyManager(m_dataControlManager);
    m_dataControlManager = nullptr;
    m_dataControlOps = nullptr;
  }
  if (m_virtualKeyboardManager != nullptr) {
    zwp_virtual_keyboard_manager_v1_destroy(m_virtualKeyboardManager);
    m_virtualKeyboardManager = nullptr;
  }
}

void ClipboardGlobals::setClipboardService(ClipboardService* clipboardService) {
  m_clipboardService = clipboardService;
  bindClipboardService();
}

void ClipboardGlobals::setVirtualKeyboardService(VirtualKeyboardService* virtualKeyboardService) {
  m_virtualKeyboardService = virtualKeyboardService;
  bindVirtualKeyboardService();
}

void ClipboardGlobals::bindClipboardService() {
  wl_seat* const seat = m_connection.seat();
  if (m_clipboardService == nullptr
      || m_dataControlManager == nullptr
      || m_dataControlOps == nullptr
      || seat == nullptr) {
    return;
  }
  m_clipboardService->bind(m_dataControlManager, m_dataControlOps, seat);
}

void ClipboardGlobals::bindVirtualKeyboardService() {
  wl_seat* const seat = m_connection.seat();
  if (m_virtualKeyboardService == nullptr || m_virtualKeyboardManager == nullptr || seat == nullptr) {
    return;
  }
  m_virtualKeyboardService->bind(m_virtualKeyboardManager, seat);
}
