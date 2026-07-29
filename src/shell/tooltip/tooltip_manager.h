#pragma once

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "shell/tooltip/tooltip_content.h"
#include "ui/signal.h"

#include <memory>

class InputArea;
class Node;
class PopupSurface;
class RenderContext;
class ConfigService;
class WaylandConnection;
struct wl_output;
struct xdg_surface;
struct zwlr_layer_surface_v1;

class TooltipManager {
public:
  static TooltipManager& instance();

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  void shutdown();
  // Destroy any live tooltip immediately. Required before destroying a parent
  // layer surface — xdg_popup must die before its parent (Jay enforces this).
  void forceDestroy();

  void onHoverChange(InputArea* area, zwlr_layer_surface_v1* parentLayerSurface, wl_output* output);
  void onHoverChange(InputArea* area, xdg_surface* parentXdgSurface, wl_output* output);
  void syncAnchor(InputArea* area);

private:
  TooltipManager() = default;

  enum class State { Idle, Pending, Showing, FadingOut };

  struct Size {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
  };

  void showPopup();
  void handleHoverChange(InputArea* area);
  void scheduleRetargetPopup();
  void scheduleReshow();
  void dismissPopup();
  void scheduleDestroyPopup();
  void destroyPopup();
  [[nodiscard]] bool canRetargetPopup() const;
  void refreshFromArea(InputArea* area);
  void refreshPopupContent();
  void scheduleProviderRefresh();
  Size measureContent(const TooltipContent& content);
  void buildScene(const TooltipContent& content, float w, float h, float opacity = 0.0f);
  void prepareFrame(bool needsUpdate, bool needsLayout);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;

  State m_state = State::Idle;
  bool m_reshowQueued = false;
  bool m_retargetQueued = false;
  bool m_destroyScheduled = false;
  bool m_showAfterDestroy = false;
  Timer m_showTimer;
  Timer m_refreshTimer;

  TooltipContent m_pendingContent;
  zwlr_layer_surface_v1* m_pendingLayerParent = nullptr;
  xdg_surface* m_pendingXdgParent = nullptr;
  wl_output* m_pendingOutput = nullptr;
  InputArea* m_pendingArea = nullptr;

  zwlr_layer_surface_v1* m_activeLayerParent = nullptr;
  xdg_surface* m_activeXdgParent = nullptr;
  wl_output* m_activeOutput = nullptr;

  std::unique_ptr<PopupSurface> m_surface;
  AnimationManager m_animations;
  std::unique_ptr<Node> m_sceneRoot;
  AnimationManager::Id m_fadeAnimId = 0;

  Signal<>::ScopedConnection m_paletteConn;
};
