#pragma once

#include "wayland/ext_foreign_toplevels.h"
#include "wayland/wayland_seat.h"
#include "wayland/wayland_toplevels.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct wl_compositor;
struct wl_display;
struct wl_output;
struct wl_registry;
struct wl_seat;
struct wl_shm;
struct wl_subcompositor;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct xdg_wm_base;
struct wp_cursor_shape_manager_v1;
struct ext_idle_notifier_v1;
struct ext_idle_notification_v1;
struct zwp_idle_inhibit_manager_v1;
struct ext_background_effect_manager_v1;
struct xdg_activation_v1;
struct ext_session_lock_manager_v1;
struct zwlr_foreign_toplevel_manager_v1;
struct zwlr_foreign_toplevel_handle_v1;
struct ext_workspace_manager_v1;
struct zdwl_ipc_manager_v2;
struct zwp_virtual_keyboard_manager_v1;
struct hyprland_focus_grab_manager_v1;
struct hyprland_toplevel_mapping_manager_v1;
struct zwlr_gamma_control_manager_v1;
struct wp_fractional_scale_manager_v1;
struct wp_viewporter;
class ClipboardService;
class FocusGrabService;
struct DataControlOps;
class VirtualKeyboardService;

struct WaylandOutput {
  std::uint32_t name = 0;
  std::string interfaceName;
  std::string connectorName;
  std::string description;
  std::uint32_t version = 0;
  wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t logicalWidth = 0;
  std::int32_t logicalHeight = 0;
  zxdg_output_v1* xdgOutput = nullptr;
  bool done = false;
};

class WaylandConnection {
public:
  WaylandConnection();
  ~WaylandConnection();

  WaylandConnection(const WaylandConnection&) = delete;
  WaylandConnection& operator=(const WaylandConnection&) = delete;

  using ChangeCallback = std::function<void()>;

  bool connect();

  // Delegate setters
  void setOutputChangeCallback(ChangeCallback callback);
  void setOutputLifecycleCallbacks(std::function<void(wl_output*)> added, std::function<void(wl_output*)> removed);
  void setWorkspaceManagerCallbacks(
      std::function<void(ext_workspace_manager_v1*)> extWorkspace, std::function<void(zdwl_ipc_manager_v2*)> dwlIpc
  );
  void setToplevelChangeCallback(ChangeCallback callback);
  void setHyprlandToplevelMappingManagerCallback(
      std::function<void(hyprland_toplevel_mapping_manager_v1* manager)> callback
  );
  void setPointerEventCallback(WaylandSeat::PointerEventCallback callback);
  void setKeyboardEventCallback(WaylandSeat::KeyboardEventCallback callback);
  /// Fired when both `ext_idle_notifier_v1` and `wl_seat` are bound (including late registry globals).
  void setIdleCapabilitiesReadyCallback(ChangeCallback callback);
  void setClipboardService(ClipboardService* clipboardService);
  void setVirtualKeyboardService(VirtualKeyboardService* virtualKeyboardService);
  void setCursorShape(std::uint32_t serial, std::uint32_t shape);

  [[nodiscard]] int repeatPollTimeoutMs() const;
  void repeatTick();
  void stopKeyRepeat();

  // Queries
  [[nodiscard]] bool isConnected() const noexcept;
  [[nodiscard]] bool hasRequiredGlobals() const noexcept;
  [[nodiscard]] bool hasLayerShell() const noexcept;
  [[nodiscard]] bool hasSubcompositor() const noexcept;
  [[nodiscard]] bool hasXdgOutputManager() const noexcept;
  [[nodiscard]] bool hasXdgShell() const noexcept;
  [[nodiscard]] bool hasExtWorkspaceManager() const noexcept;
  [[nodiscard]] bool hasDwlIpcManager() const noexcept;
  [[nodiscard]] bool hasForeignToplevelManager() const noexcept;
  [[nodiscard]] bool hasExtForeignToplevelList() const noexcept;
  [[nodiscard]] bool hasSessionLockManager() const noexcept;
  [[nodiscard]] bool hasIdleNotifier() const noexcept;
  [[nodiscard]] bool hasIdleInhibitManager() const noexcept;
  [[nodiscard]] bool hasFractionalScale() const noexcept;
  [[nodiscard]] bool hasGammaControl() const noexcept;
  [[nodiscard]] bool hasBackgroundEffectBlur() const noexcept;
  [[nodiscard]] zwlr_gamma_control_manager_v1* gammaControlManager() const noexcept;
  [[nodiscard]] ext_background_effect_manager_v1* backgroundEffectManager() const noexcept;
  [[nodiscard]] wp_fractional_scale_manager_v1* fractionalScaleManager() const noexcept;
  [[nodiscard]] hyprland_focus_grab_manager_v1* hyprlandFocusGrabManager() const noexcept;
  [[nodiscard]] FocusGrabService* focusGrabService() const noexcept;
  [[nodiscard]] wp_viewporter* viewporter() const noexcept;
  [[nodiscard]] wl_display* display() const noexcept;
  [[nodiscard]] std::string describeDisplayError(int operationErrno = 0) const;
  [[nodiscard]] wl_compositor* compositor() const noexcept;
  [[nodiscard]] wl_seat* seat() const noexcept;
  [[nodiscard]] wl_shm* shm() const noexcept;
  [[nodiscard]] wl_subcompositor* subcompositor() const noexcept;
  [[nodiscard]] zwlr_layer_shell_v1* layerShell() const noexcept;
  [[nodiscard]] xdg_wm_base* xdgWmBase() const noexcept;
  [[nodiscard]] ext_session_lock_manager_v1* sessionLockManager() const noexcept;
  [[nodiscard]] ext_idle_notifier_v1* idleNotifier() const noexcept;
  /// Inhibitor-aware idle notification (`get_idle_notification`); honors `zwp_idle_inhibitor_v1`.
  [[nodiscard]] ext_idle_notification_v1* createIdleNotification(std::uint32_t timeoutMs) const;
  [[nodiscard]] zwp_idle_inhibit_manager_v1* idleInhibitManager() const noexcept;
  [[nodiscard]] const std::vector<WaylandOutput>& outputs() const noexcept;
  [[nodiscard]] WaylandOutput* findOutputByWl(wl_output* wlOutput);
  [[nodiscard]] const WaylandOutput* findOutputByWl(wl_output* wlOutput) const;
  [[nodiscard]] WaylandOutput* findOutputByXdg(zxdg_output_v1* xdgOutput);

  [[nodiscard]] bool hasXdgActivation() const noexcept;
  [[nodiscard]] std::string requestActivationToken(wl_surface* surface) const;
  void activateSurface(wl_surface* surface);

  [[nodiscard]] std::optional<ActiveToplevel> activeToplevel() const;
  [[nodiscard]] std::optional<ActiveToplevel>
  matchToplevelByTitleAndAppId(std::string_view title, std::string_view appId, wl_output* preferredOutput) const;
  [[nodiscard]] wl_output* activeToplevelOutput() const;
  [[nodiscard]] std::vector<std::string> runningAppIds(wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<ToplevelInfo>
  windowsForApp(const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<ToplevelInfo>
  extWindowsForApp(const std::string& idLower, const std::string& wmClassLower) const;
  template <typename Fn> void visitExtToplevelHandles(Fn&& fn) const {
    m_extForeignToplevels.visitExtHandles(std::forward<Fn>(fn));
  }
  void activateToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  void closeToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  template <typename Fn> void visitWlrToplevelHandles(Fn&& fn) const {
    m_toplevelsHandler.visitWlrHandles(std::forward<Fn>(fn));
  }
  [[nodiscard]] wl_output* lastPointerOutput() const noexcept;
  [[nodiscard]] wl_surface* lastPointerSurface() const noexcept;
  [[nodiscard]] wl_surface* lastKeyboardSurface() const noexcept;
  [[nodiscard]] bool hasPointerPosition() const noexcept;
  [[nodiscard]] double lastPointerX() const noexcept;
  [[nodiscard]] double lastPointerY() const noexcept;
  [[nodiscard]] WaylandSeat::InputSource lastInputSource() const noexcept;
  [[nodiscard]] std::string currentKeyboardLayoutName() const;
  [[nodiscard]] std::vector<std::string> keyboardLayoutNames() const;
  [[nodiscard]] WaylandSeat::LockKeysState keyboardLockKeysState() const;
  [[nodiscard]] std::uint32_t lastInputSerial() const noexcept;
  [[nodiscard]] double userIdleSeconds() const noexcept;
  [[nodiscard]] bool hasFreshPointerOutput(std::chrono::milliseconds maxAge) const noexcept;
  [[nodiscard]] wl_output* outputForSurface(wl_surface* surface) const noexcept;

  void registerSurfaceOutput(wl_surface* surface, wl_output* output);
  void registerLayerSurface(wl_surface* surface, zwlr_layer_surface_v1* layerSurface);
  void unregisterSurface(wl_surface* surface);
  [[nodiscard]] zwlr_layer_surface_v1* layerSurfaceFor(wl_surface* surface) const noexcept;
  void notifyOutputReady(wl_output* output);

  // Registry listener entrypoints
  static void
  handleGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version);
  static void handleGlobalRemove(void* data, wl_registry* registry, std::uint32_t name);

  void onBackgroundEffectCapabilities(std::uint32_t capabilities) noexcept;
  void notifyIdleCapabilitiesReady();

private:
  void bindGlobal(wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version);
  void bindClipboardService();
  void bindVirtualKeyboardService();
  void cleanup();
  void logStartupSummary() const;

  wl_display* m_display = nullptr;
  wl_registry* m_registry = nullptr;
  wl_compositor* m_compositor = nullptr;
  wl_seat* m_seat = nullptr;
  wl_shm* m_shm = nullptr;
  wl_subcompositor* m_subcompositor = nullptr;
  zwlr_layer_shell_v1* m_layerShell = nullptr;
  zxdg_output_manager_v1* m_xdgOutputManager = nullptr;
  xdg_wm_base* m_xdgWmBase = nullptr;
  wp_cursor_shape_manager_v1* m_cursorShapeManager = nullptr;
  xdg_activation_v1* m_xdgActivation = nullptr;
  ext_session_lock_manager_v1* m_sessionLockManager = nullptr;
  ext_idle_notifier_v1* m_idleNotifier = nullptr;
  zwp_idle_inhibit_manager_v1* m_idleInhibitManager = nullptr;
  ext_background_effect_manager_v1* m_backgroundEffectManager = nullptr;
  wp_fractional_scale_manager_v1* m_fractionalScaleManager = nullptr;
  hyprland_focus_grab_manager_v1* m_hyprlandFocusGrabManager = nullptr;
  zwlr_gamma_control_manager_v1* m_gammaControlManager = nullptr;
  std::unique_ptr<FocusGrabService> m_focusGrabService;
  wp_viewporter* m_viewporter = nullptr;
  bool m_backgroundEffectBlurSupported = false;
  void* m_dataControlManager = nullptr;
  const DataControlOps* m_dataControlOps = nullptr;
  zwp_virtual_keyboard_manager_v1* m_virtualKeyboardManager = nullptr;
  ClipboardService* m_clipboardService = nullptr;
  VirtualKeyboardService* m_virtualKeyboardService = nullptr;
  bool m_hasLayerShellGlobal = false;
  bool m_hasExtWorkspaceGlobal = false;
  bool m_hasDwlIpcGlobal = false;
  bool m_hasForeignToplevelManagerGlobal = false;
  bool m_hasExtForeignToplevelListGlobal = false;
  std::vector<WaylandOutput> m_outputs;
  ChangeCallback m_outputChangeCallback;
  ChangeCallback m_idleCapabilitiesReadyCallback;
  std::function<void(wl_output*)> m_outputAddedCallback;
  std::function<void(wl_output*)> m_outputRemovedCallback;
  std::function<void(ext_workspace_manager_v1*)> m_extWorkspaceManagerCallback;
  std::function<void(zdwl_ipc_manager_v2*)> m_dwlIpcManagerCallback;
  std::function<void(hyprland_toplevel_mapping_manager_v1*)> m_hyprlandToplevelMappingManagerCallback;
  std::unordered_map<wl_surface*, wl_output*> m_surfaceOutputMap;
  std::unordered_map<wl_surface*, zwlr_layer_surface_v1*> m_layerSurfaceMap;
  wl_output* m_lastPointerOutput = nullptr;
  std::chrono::steady_clock::time_point m_lastPointerOutputAt{};
  WaylandSeat::PointerEventCallback m_pointerEventCallback;

  WaylandSeat m_seatHandler;
  WaylandToplevels m_toplevelsHandler;
  WaylandExtForeignToplevels m_extForeignToplevels;
};
