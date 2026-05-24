#pragma once

#include "config/config_types.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu.h"
#include "ui/popup_chrome.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class Node;
class PopupSurface;
class RenderContext;
class WaylandConnection;
struct PointerEvent;
struct wl_output;
struct wl_surface;
struct xdg_surface;
struct zwlr_layer_surface_v1;

struct ContextMenuPopupPlacement {
  std::uint32_t anchor = 0;
  std::uint32_t gravity = 0;
  std::int32_t offsetX = 0;
  std::int32_t offsetY = 0;
  popup_chrome::Attachment chromeAttachment{
      .horizontal = popup_chrome::HorizontalAttachment::Center,
      .vertical = popup_chrome::VerticalAttachment::Top,
  };
};

class ContextMenuPopup {
public:
  ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext);
  ~ContextMenuPopup();

  void open(
      std::vector<ContextMenuControlEntry> entries, float menuWidth, std::size_t maxVisible, std::int32_t anchorX,
      std::int32_t anchorY, std::int32_t anchorW, std::int32_t anchorH, zwlr_layer_surface_v1* parentLayerSurface,
      wl_output* output, const ContextMenuPopupPlacement* placement = nullptr
  );
  void openAsChild(
      std::vector<ContextMenuControlEntry> entries, float menuWidth, std::size_t maxVisible, std::int32_t anchorX,
      std::int32_t anchorY, std::int32_t anchorW, std::int32_t anchorH, xdg_surface* parentXdgSurface,
      wl_output* output, const ContextMenuPopupPlacement* placement = nullptr
  );
  void close();
  [[nodiscard]] bool isOpen() const noexcept;

  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback);
  void setOnDismissed(std::function<void()> callback);
  void setShadowConfig(const ShellConfig::ShadowConfig& shadow);

  bool onPointerEvent(const PointerEvent& event);
  [[nodiscard]] wl_surface* wlSurface() const noexcept;

private:
  void openCommon(
      std::vector<ContextMenuControlEntry> entries, float menuWidth, std::size_t maxVisible, std::int32_t anchorX,
      std::int32_t anchorY, std::int32_t anchorW, std::int32_t anchorH, zwlr_layer_surface_v1* parentLayerSurface,
      xdg_surface* parentXdgSurface, wl_output* output, const ContextMenuPopupPlacement* placement
  );

  WaylandConnection& m_wayland;
  RenderContext& m_renderContext;
  std::unique_ptr<PopupSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  InputDispatcher m_inputDispatcher;
  wl_surface* m_wlSurface = nullptr;
  bool m_pointerInside = false;

  std::function<void(const ContextMenuControlEntry&)> m_onActivate;
  std::function<void()> m_onDismissed;
  ShellConfig::ShadowConfig m_shadowConfig;
};
