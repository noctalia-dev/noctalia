#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "ui/popup_chrome.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class CompositorPlatform;
class ConfigService;
class Node;
class PopupSurface;
class RenderContext;
struct DesktopAction;
struct DesktopEntry;
struct DockConfig;
enum class DockEdge : std::uint8_t;
struct PointerEvent;
struct PopupSurfaceConfig;
struct ToplevelInfo;
struct wl_output;
struct wl_surface;
struct zwlr_foreign_toplevel_handle_v1;
struct zwlr_layer_surface_v1;

namespace shell::dock {

  struct DockPopup {
    DockPopup();
    ~DockPopup();

    AnimationManager animations;
    std::unique_ptr<PopupSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    popup_chrome::Geometry chrome;
    // Stack popups slide out from the dock edge; hidden while one of their files is dragged.
    DockEdge edge{};
    float revealProgress = 1.0F;
    AnimationManager::Id revealAnimId = 0;
    bool hidden = false;
    InputDispatcher inputDispatcher;
    wl_surface* wlSurface = nullptr;
    bool pointerInside = false;
    std::vector<zwlr_foreign_toplevel_handle_v1*> handles;
    std::vector<ToplevelInfo> windows;
  };

  struct DockMenuCallbacks {
    std::function<void(std::size_t windowIndex)> activateWindow;
    std::function<void(std::size_t windowIndex)> closeWindow;
    std::function<void(std::size_t windowIndex, bool minimized)> setWindowMinimized;
    std::function<void(const DesktopAction&)> launchAction;
    std::function<void(bool pinned)> setEntryPinned;
    std::function<void()> closeMenu;
  };

  // Dock surface rect a popup attaches to.
  struct DockPopupAnchor {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 1;
    std::int32_t height = 1;
  };

  // Places a popup of `chrome` size next to `anchor`, or the dock item under the pointer without one.
  [[nodiscard]] PopupSurfaceConfig dockPopupSurfaceConfig(
      CompositorPlatform& platform, const DockConfig& dockConfig, const popup_chrome::Geometry& chrome,
      const std::optional<DockPopupAnchor>& anchor = std::nullopt
  );

  // Route a pointer event to an open popup; returns true if consumed.
  bool routePopupEvent(DockPopup& popup, const PointerEvent& event);

  [[nodiscard]] std::unique_ptr<DockPopup> createItemMenu(
      CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext,
      zwlr_layer_surface_v1* parentLayerSurface, wl_output* output, const DockConfig& dockConfig,
      const DesktopEntry& entry, const std::vector<ToplevelInfo>& windows, const DockMenuCallbacks& callbacks
  );

} // namespace shell::dock
