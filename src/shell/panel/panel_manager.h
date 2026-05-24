#pragma once

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/panel/attached_panel_context.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_click_shield.h"
#include "ui/dialogs/layer_popup_host.h"
#include "wayland/hyprland/focus_grab_service.h"
#include "wayland/hyprland/popup_grab_host.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"
#include "wayland/wayland_seat.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class ConfigService;
class CompositorPlatform;
class ContextMenuPopup;
class SelectDropdownPopup;
class Box;
class IpcService;
class Renderer;
class RenderContext;
class WaylandConnection;
struct PointerEvent;
struct wl_output;
struct wl_surface;

struct PanelOpenRequest {
  wl_output* output = nullptr;
  float anchorX = 0.0f;
  float anchorY = 0.0f;
  bool hasExplicitAnchor = false;
  bool hasAnchorPosition = false;
  std::string_view context = {};
  std::string_view sourceBarName = {};
};

class PanelManager : public PopupGrabHost {
public:
  PanelManager();
  ~PanelManager();

  PanelManager(const PanelManager&) = delete;
  PanelManager& operator=(const PanelManager&) = delete;

  static PanelManager& instance();
  static PanelManager* current() noexcept;

  void initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext);

  // Optional: invoked from shell UI (e.g. control center) to spawn the standalone settings toplevel.
  void setOpenSettingsWindowCallback(std::function<void()> callback);
  void setToggleSettingsWindowCallback(std::function<void()> callback);
  void openSettingsWindow();
  void toggleSettingsWindow();
  void setAttachedPanelGeometryCallback(
      std::function<void(wl_output*, std::string_view, std::optional<AttachedPanelGeometry>)> callback
  );
  // Callback to query the bar surface rects on a given output, in output-local
  // coordinates. The click shield's input region excludes these rects so
  // clicks on bar widgets keep flowing to the bar while a panel is open.
  void setClickShieldExcludeRectsProvider(std::function<std::vector<InputRect>(wl_output*)> provider);
  // Callback returning every bar wl_surface. Used to seed the Hyprland focus
  // grab whitelist so bar widgets keep receiving clicks while a panel is open.
  void setFocusGrabBarSurfacesProvider(std::function<std::vector<wl_surface*>()> provider);
  void setPanelClosedCallback(std::function<void()> callback);

  void registerPanel(const std::string& id, std::unique_ptr<Panel> content);

  void openPanel(const std::string& panelId, PanelOpenRequest request = {});
  void closePanel();
  void togglePanel(const std::string& panelId, PanelOpenRequest request);
  // IPC-friendly overload: asks CompositorPlatform for preferred interactive output.
  void togglePanel(const std::string& panelId);

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

  [[nodiscard]] bool isOpen() const noexcept;
  [[nodiscard]] bool isOpenPanel(std::string_view panelId) const noexcept;
  [[nodiscard]] bool isPanelTransitionActive() const noexcept;
  [[nodiscard]] bool isAttachedOpen() const noexcept;
  // Bar that opened the active panel; empty when none was recorded.
  [[nodiscard]] std::string_view attachedSourceBarName() const noexcept;
  [[nodiscard]] const std::string& activePanelId() const noexcept;
  // True when a panel is open and it reports the given context as active (e.g. control-center tab).
  [[nodiscard]] bool isActivePanelContext(std::string_view context) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> fallbackPopupParentContext() const noexcept;

  [[nodiscard]] RenderContext* renderContext() const noexcept { return m_renderContext; }
  [[nodiscard]] WaylandConnection* wayland() const noexcept;

  void setActivePopup(ContextMenuPopup* popup);
  void clearActivePopup();

  void refresh();
  // Reacts to a ConfigService reload while a panel is open: re-pulls the host bar's
  // per-panel-relevant config (attached background opacity) and
  // re-applies the compositor blur region (which depends on shell.panel.background_blur,
  // affects both attached and layer-shell panels). No-op when no panel is open.
  void onConfigReloaded();
  void onIconThemeChanged();
  void requestUpdateOnly();
  void requestLayout();
  // Requests a redraw on the active panel surface without re-running panel
  // update/layout. Used for reactive palette restyling.
  void requestRedraw();
  void requestFrameTick();
  void close();
  void beginAttachedPopup(wl_surface* surface);
  void endAttachedPopup(wl_surface* surface);

  // PopupGrabHost. PopupSurface enrolls itself with us while our focus_grab
  // is active so the compositor doesn't fire `cleared` when the user
  // interacts with a popup opened from inside the panel.
  void registerPopupSurface(wl_surface* surface) override;
  void unregisterPopupSurface(wl_surface* surface) override;

  void registerIpc(IpcService& ipc);

private:
  static PanelManager* s_instance;

  void buildScene(std::uint32_t width, std::uint32_t height);
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void destroyPanel();
  // Called BEFORE the panel surface commits so shields sit below the panel
  // within the layer-shell layer. No-op when the focus-grab path is in use.
  void activateClickShield();
  // Called AFTER the panel surface is mapped so the panel wl_surface is
  // available for the whitelist. No-op when focus-grab is unavailable.
  void activateFocusGrab();
  void deactivateOutsideClickHandlers();
  void applyAttachedReveal(float progress);
  void applyDetachedReveal(float progress);
  void startAttachedOpenAnimation();
  void publishAttachedPanelGeometry(float revealProgress);
  // Restyle the attached-panel decoration nodes (bg fill, drop shadow, contact shadow)
  // using the cached attached background opacity and bar position. Geometry/positions are not touched.
  // Safe to call any time after buildScene has run.
  void applyAttachedDecorationStyle();
  // Submit a wl_region matching the visible panel body to the compositor for blur.
  // Honors shell.panel.background_blur; clips by m_attachedRevealProgress so the blur
  // grows in lock-step with the open/close animation.
  void applyPanelCompositorBlur();

  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::function<void()> m_openSettingsWindow;
  std::function<void()> m_toggleSettingsWindow;
  std::function<void(wl_output*, std::string_view, std::optional<AttachedPanelGeometry>)>
      m_attachedPanelGeometryCallback;
  std::function<std::vector<InputRect>(wl_output*)> m_clickShieldExcludeRectsProvider;
  std::function<std::vector<wl_surface*>()> m_focusGrabBarSurfacesProvider;
  std::function<void()> m_panelClosedCallback;
  PanelClickShield m_clickShield;
  std::unique_ptr<FocusGrab> m_focusGrab;

  std::unique_ptr<Surface> m_surface;
  LayerSurface* m_layerSurface = nullptr;
  // m_sceneRoot must be destroyed before m_animations — ~Node() calls cancelForOwner().
  // Also m_panels (which own their own Nodes parented under m_sceneRoot) must be destroyed
  // before m_animations for the same reason.
  AnimationManager m_animations;
  std::unique_ptr<Node> m_sceneRoot;
  Node* m_bgNode = nullptr;
  Node* m_contentNode = nullptr;
  Node* m_attachedRevealClipNode = nullptr;
  Node* m_attachedRevealContentNode = nullptr;
  Box* m_panelShadowNode = nullptr;
  Box* m_panelContactShadowNode = nullptr;
  InputDispatcher m_inputDispatcher;

  std::unordered_map<std::string, std::unique_ptr<Panel>> m_panels;
  Panel* m_activePanel = nullptr;
  std::string m_activePanelId;
  std::string m_pendingOpenContext;

  wl_output* m_output = nullptr;
  wl_surface* m_wlSurface = nullptr;
  float m_contentWidth = 0.0f;
  float m_contentHeight = 0.0f;
  std::int32_t m_panelInsetX = 0;
  std::int32_t m_panelInsetY = 0;
  std::uint32_t m_panelVisualWidth = 0;
  std::uint32_t m_panelVisualHeight = 0;
  float m_attachedBackgroundOpacity = 1.0f;
  bool m_attachedContactShadow = false;
  float m_attachedRevealProgress = 1.0f;
  float m_detachedRevealProgress = 1.0f;
  AttachedRevealDirection m_attachedRevealDirection = AttachedRevealDirection::Down;
  Timer m_keyboardRelaxTimer;
  std::string m_attachedBarPosition; // "top" / "bottom" / "left" / "right" while attached, empty otherwise
  std::string m_sourceBarName;       // name of the bar that opened the current panel
  std::optional<AttachedPanelGeometry> m_attachedPanelGeometry;
  bool m_pointerInside = false;
  bool m_inTransition = false;
  bool m_closing = false;
  bool m_attachedToBar = false;
  bool m_attachedOpenAnimationPending = false;
  std::size_t m_attachedPopupCount = 0;
  ContextMenuPopup* m_activePopup = nullptr;
  std::unique_ptr<SelectDropdownPopup> m_selectPopup;
  std::uint64_t m_destroyGeneration = 0; // invalidates stale deferred destroyPanel calls
};
