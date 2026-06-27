#pragma once

#include "render/scene/input_dispatcher.h"
#include "ui/popup_chrome.h"

#include <functional>
#include <memory>
#include <vector>

class CompositorPlatform;
class ConfigService;
class Node;
class PopupSurface;
class RenderContext;
struct DesktopAction;
struct DesktopEntry;
struct DockConfig;
struct PointerEvent;
struct ToplevelInfo;
struct wl_output;
struct wl_surface;
struct zwlr_foreign_toplevel_handle_v1;
struct zwlr_layer_surface_v1;

namespace shell::dock {

  struct DockPopup {
    DockPopup();
    ~DockPopup();

    std::unique_ptr<PopupSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    popup_chrome::Geometry chrome;
    InputDispatcher inputDispatcher;
    wl_surface* wlSurface = nullptr;
    bool pointerInside = false;
    std::vector<zwlr_foreign_toplevel_handle_v1*> handles;
    std::vector<ToplevelInfo> windows;
  };

  struct DockMenuCallbacks {
    std::function<void(std::size_t windowIndex)> activateWindow;
    std::function<void(std::size_t windowIndex)> closeWindow;
    std::function<void(const DesktopAction&)> launchAction;
    std::function<void(bool pinned)> setEntryPinned;
    std::function<void()> closeMenu;
  };

  // Route a pointer event to an open popup; returns true if consumed.
  bool routePopupEvent(DockPopup& popup, const PointerEvent& event);

  [[nodiscard]] std::unique_ptr<DockPopup> createItemMenu(
      CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext,
      zwlr_layer_surface_v1* parentLayerSurface, wl_output* output, const DockConfig& dockConfig,
      const DesktopEntry& entry, const std::vector<ToplevelInfo>& windows, const DockMenuCallbacks& callbacks
  );

} // namespace shell::dock
