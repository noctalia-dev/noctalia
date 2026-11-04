#pragma once

#include "wayland/wayland_extension.h"

class ClipboardService;
class VirtualKeyboardService;
class WaylandConnection;
struct DataControlOps;
struct zwp_virtual_keyboard_manager_v1;

// Data-control (ext preferred over wlr) and virtual-keyboard managers, handed to the clipboard and paste services
// once the seat is bound.
class ClipboardGlobals final : public WaylandExtension {
public:
  explicit ClipboardGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  void setClipboardService(ClipboardService* clipboardService);
  void setVirtualKeyboardService(VirtualKeyboardService* virtualKeyboardService);

private:
  void bindClipboardService();
  void bindVirtualKeyboardService();

  WaylandConnection& m_connection;
  void* m_dataControlManager = nullptr;
  const DataControlOps* m_dataControlOps = nullptr;
  zwp_virtual_keyboard_manager_v1* m_virtualKeyboardManager = nullptr;
  ClipboardService* m_clipboardService = nullptr;
  VirtualKeyboardService* m_virtualKeyboardService = nullptr;
};
