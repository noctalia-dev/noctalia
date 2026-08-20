#pragma once

#include "core/timer_manager.h"
#include "shell/panel/panel_click_shield.h"
#include "ui/controls/context_menu.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

class ContextMenuPopup;
class RenderContext;
class WaylandConnection;
struct PointerEvent;

struct TransientContextMenuRequest {
  std::vector<ContextMenuControlEntry> entries;
  std::size_t maxVisible = 12;
  float contentScale = 1.0F;
  float maxMenuWidth = 0.0F;
};

// A compositor-independent context menu opened without an originating Wayland
// input event (for example from IPC). A transparent layer-shell surface on each
// output discovers the cursor position through pointer-enter and remains as the
// modal outside-click catcher while the xdg_popup is open.
class TransientContextMenu {
public:
  using ActivateCallback = std::function<void(const ContextMenuControlEntry&)>;

  TransientContextMenu() = default;
  ~TransientContextMenu();

  TransientContextMenu(const TransientContextMenu&) = delete;
  TransientContextMenu& operator=(const TransientContextMenu&) = delete;

  void initialize(WaylandConnection& wayland, RenderContext& renderContext);
  [[nodiscard]] bool open(TransientContextMenuRequest request, ActivateCallback onActivate);
  void close();

  [[nodiscard]] bool isActive() const noexcept;
  bool onPointerEvent(const PointerEvent& event);

private:
  void openPopup(const PointerEvent& event);

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  PanelClickShield m_clickShield;
  std::unique_ptr<ContextMenuPopup> m_popup;
  std::unique_ptr<TransientContextMenuRequest> m_pendingRequest;
  ActivateCallback m_onActivate;
  Timer m_pointerWaitTimer;
  bool m_closing = false;
};
