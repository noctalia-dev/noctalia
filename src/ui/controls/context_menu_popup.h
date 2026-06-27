#pragma once

#include "config/config_types.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu.h"
#include "ui/popup_chrome.h"
#include "ui/popup_parent.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Node;
class PopupSurface;
class RenderContext;
class WaylandConnection;
struct PointerEvent;
struct wl_surface;

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

struct ContextMenuPopupRequest {
  std::vector<ContextMenuControlEntry> entries;
  float menuWidth = 0.0f;
  std::size_t maxVisible = 0;
  PopupAnchorRect anchor;
  PopupSurfaceParent parent;
  std::optional<ContextMenuPopupPlacement> placement = std::nullopt;
};

class ContextMenuPopup {
public:
  ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext);
  ~ContextMenuPopup();

  void open(ContextMenuPopupRequest request);
  void close();
  [[nodiscard]] bool isOpen() const noexcept;

  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback);
  void setOnDismissed(std::function<void()> callback);
  void setShadowConfig(const ShellConfig::ShadowConfig& shadow);

  bool onPointerEvent(const PointerEvent& event);
  [[nodiscard]] wl_surface* wlSurface() const noexcept;

private:
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
