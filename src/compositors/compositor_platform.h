#pragma once

#include "compositors/keyboard_backend.h"
#include "compositors/workspace_backend.h"
#include "wayland/wayland_connection.h"

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

struct wl_output;
struct wl_surface;
struct ext_workspace_manager_v1;
struct org_kde_plasma_virtual_desktop_management;
struct zdwl_ipc_manager_v2;
struct hyprland_toplevel_mapping_manager_v1;
struct zwlr_foreign_toplevel_handle_v1;
struct ext_foreign_toplevel_handle_v1;
class OutputProbe;
class SessionBus;
class WaylandWorkspaces;

namespace compositors::kde {
  class KwinActiveWindow;
}

namespace compositors::hyprland {
  class HyprlandToplevelMapping;
}

namespace compositors {
  class CompositorRuntimeRegistry;
  class FocusedOutputBackend;
  class OutputPowerBackend;
  namespace niri {
    class NiriRuntime;
  }
  namespace umbriel {
    class UmbrielRuntime;
  }
} // namespace compositors

class WorkspaceAlertService;

class CompositorPlatform {
public:
  using ChangeCallback = std::function<void()>;

  explicit CompositorPlatform(WaylandConnection& wayland);
  ~CompositorPlatform();

  CompositorPlatform(const CompositorPlatform&) = delete;
  CompositorPlatform& operator=(const CompositorPlatform&) = delete;

  void initialize();
  void startKdeActiveWindow(SessionBus& bus);
  void cleanup();

  WaylandConnection& wayland() noexcept { return m_wayland; }
  const WaylandConnection& wayland() const noexcept { return m_wayland; }
  wl_display* display() const noexcept;
  bool hasXdgShell() const noexcept;
  bool hasXdgActivation() const noexcept;
  std::string requestActivationToken(wl_surface* surface) const;
  bool hasGammaControl() const noexcept;
  const std::vector<WaylandOutput>& outputs() const noexcept;
  const WaylandOutput* findOutputByWl(wl_output* output) const;
  wl_output* outputForSurface(wl_surface* surface) const noexcept;
  FocusGrabService* focusGrabService() const noexcept;
  bool hasPointerPosition() const noexcept;
  wl_surface* lastPointerSurface() const noexcept;
  wl_surface* lastKeyboardSurface() const noexcept;
  double lastPointerX() const noexcept;
  double lastPointerY() const noexcept;
  std::uint32_t lastInputSerial() const noexcept;
  zwlr_layer_surface_v1* layerSurfaceFor(wl_surface* surface) const noexcept;
  void stopKeyRepeat();
  void setCursorShape(std::uint32_t serial, std::uint32_t shape);

  // Output resolved from a real focus source (compositor IPC, focused-output
  // backend, active toplevel, keyboard/fresh-pointer surface). Returns nullptr
  // when no such source is available — callers that need an output regardless
  // should use preferredInteractiveOutput() or probeFocusedOutput().
  wl_output* focusedInteractiveOutput(std::chrono::milliseconds pointerMaxAge = std::chrono::milliseconds(1200)) const;

  wl_output*
  preferredInteractiveOutput(std::chrono::milliseconds pointerMaxAge = std::chrono::milliseconds(1200)) const;

  // Asks the compositor which output a NULL-output layer surface lands on (the
  // focused one) by mapping a throwaway 1x1 probe surface, then reports it. For
  // compositors with no focus source, where focusedInteractiveOutput() is null.
  // The callback receives the probed output, or nullptr on timeout. Only one
  // probe runs at a time; a new call cancels any in-flight probe.
  void probeFocusedOutput(std::function<void(wl_output*)> callback, std::chrono::milliseconds timeout);

  std::optional<ActiveToplevel> activeToplevel() const;
  wl_output* activeToplevelOutput() const;
  std::vector<std::string> runningAppIds(wl_output* outputFilter = nullptr) const;
  std::vector<ToplevelInfo>
  windowsForApp(const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter = nullptr) const;
  std::vector<ToplevelInfo> windowsWithoutAppId(wl_output* outputFilter = nullptr) const;
  std::vector<ToplevelInfo> enrichedWindowsForApp(
      const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter = nullptr
  ) const;
  std::vector<ToplevelInfo> enrichedWindowsWithoutAppId(wl_output* outputFilter = nullptr) const;
  // True when taskbar windows carry exact compositor-assigned identities.
  bool hasExactWindowIdentity() const noexcept;
  bool containsWlrToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) const;
  void activateToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  void activateToplevelForAppId(std::string_view appId);
  void activateToplevelInfo(const ToplevelInfo& window);
  // Primary click on one window; multi-window cycling continues to use activation.
  void activateOrMinimizeToplevelInfo(const ToplevelInfo& window);
  std::optional<WindowMinimizeState> windowMinimizeState(const ToplevelInfo& window) const;
  [[nodiscard]] bool minimizeToplevelInfo(const ToplevelInfo& window);
  std::optional<bool> hasUnminimizedToplevels(wl_output* outputFilter) const;
  void closeToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  void closeToplevelInfo(const ToplevelInfo& window);
  void focusCompositorWindow(const std::string& windowId, bool warpPointer = false) const;

  // Focus `output` before spawning (Hyprland) so clients follow the launch monitor.
  void prepareAppLaunchOnOutput(wl_output* output);

  // Move a toplevel onto `output` when supported (Hyprland).
  void moveToplevelToOutput(const ToplevelInfo& window, wl_output* output);

  void activateKdeWindow(const std::string& title, const std::string& appId, const std::string& uuid = {});

  void setToplevelChangeCallback(ChangeCallback callback);
  void bindHyprlandToplevelMappingManager(hyprland_toplevel_mapping_manager_v1* manager);
  void syncHyprlandToplevelMappings();
  std::optional<std::string> compositorWindowIdForToplevel(zwlr_foreign_toplevel_handle_v1* handle) const;
  std::optional<std::string> compositorWindowIdForExtToplevel(ext_foreign_toplevel_handle_v1* handle) const;
  std::optional<std::string> compositorWindowIdForToplevelInfo(const ToplevelInfo& info) const;
  zwlr_foreign_toplevel_handle_v1* toplevelHandleForCompositorWindowId(std::string_view windowId) const;
  bool isCompositorWindowIdKnown(std::string_view windowId) const;
  std::optional<std::string> focusedCompositorWindowId() const;
  // Live ext handle for a compositor window id (Hyprland mapping, or the committed ext identifier
  // on compositors that publish one), nullptr when unknown.
  ext_foreign_toplevel_handle_v1* extHandleForCompositorWindowId(std::string_view windowId) const;
  // Focused window's ext-foreign-toplevel identifier. Separate from focusedCompositorWindowId(),
  // whose values drive focus/close actions and the taskbar's active-window match.
  std::optional<std::string> focusedExtToplevelIdentifier() const;
  // True when the focused window can be named exactly, so a failed lookup means the join is
  // still pending rather than that the title fallback should be tried.
  bool hasExactExtToplevelIdentity() const noexcept;

  void setWorkspaceChangeCallback(ChangeCallback callback);
  void setOverviewChangeCallback(ChangeCallback callback);
  void activateWorkspace(const std::string& id);
  void activateWorkspace(wl_output* output, const std::string& id);
  void activateWorkspace(wl_output* output, const Workspace& workspace);
  std::size_t addWorkspacePollFds(std::vector<pollfd>& fds) const;
  int workspacePollTimeoutMs() const noexcept;
  void dispatchWorkspacePoll(const std::vector<pollfd>& fds, std::size_t startIdx);
  std::vector<Workspace> workspaces() const;
  std::vector<Workspace> workspaces(wl_output* output) const;
  std::unordered_map<std::string, std::vector<std::string>> appIdsByWorkspace(wl_output* outputFilter = nullptr) const;
  std::vector<std::string> workspaceDisplayKeys(wl_output* outputFilter = nullptr) const;
  std::vector<WorkspaceWindowAssignment> workspaceWindowAssignments(wl_output* outputFilter = nullptr) const;
  // Workspace keys of the overlay workspaces currently open on `outputFilter`
  // (nullptr = every output). Empty unless the active workspace backend tracks them.
  std::vector<std::string> openOverlayWorkspaceKeys(wl_output* outputFilter = nullptr) const;

  // Workspace alerts: user-requested "attention" markers overlaid onto the
  // workspace model by reusing Workspace::id (no new per-backend identifier).
  void setWorkspaceAlertService(WorkspaceAlertService* service) noexcept;
  bool isKnownWorkspaceAlertKey(std::string_view workspaceId) const;
  std::optional<std::string> workspaceAlertKeyForWindow(std::string_view windowId) const;
  std::size_t clearActiveWorkspaceAlerts(wl_output* output);
  std::size_t clearActiveWorkspaceAlerts();
  TaskbarAssignmentMode taskbarAssignmentMode() const noexcept;
  bool supportsTaskbarWorkspaceGrouping() const noexcept;
  std::unordered_map<std::uintptr_t, WorkspaceWindow>
  assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& windows, wl_output* outputFilter = nullptr) const;
  const char* workspaceBackendName() const noexcept;

  bool cycleKeyboardLayout() const;
  bool hasKeyboardLayoutBackend() const noexcept;
  std::optional<KeyboardLayoutState> keyboardLayoutState() const;
  std::string currentKeyboardLayoutName() const;
  std::vector<std::string> keyboardLayoutNames() const;

  void setKeyboardLayoutChangeCallback(ChangeCallback callback);
  void addKeyboardLayoutPollFds(std::vector<pollfd>& fds) const;
  void dispatchKeyboardLayoutPoll(const std::vector<pollfd>& fds, std::size_t startIdx);

  bool requestSessionExit() const;
  bool setOutputPower(bool on) const;
  void setOutputPowerOffPending(bool pending) noexcept { m_outputPowerOffPending = pending; }
  bool shouldSuppressLockTransition() const;
  void setOutputPowerChangeCallback(const ChangeCallback& callback);

  bool tracksOverviewState() const noexcept;
  bool hasOverviewState() const noexcept;
  bool isOverviewOpen() const noexcept;

  bool closeOverview() const;

  compositors::niri::NiriRuntime& niriRuntime() noexcept;
  const compositors::niri::NiriRuntime& niriRuntime() const noexcept;

  compositors::umbriel::UmbrielRuntime& umbrielRuntime() noexcept;
  const compositors::umbriel::UmbrielRuntime& umbrielRuntime() const noexcept;

private:
  struct WorkspaceModelSnapshot {
    std::uint32_t outputName = 0;
    std::vector<Workspace> workspaces;
    std::vector<WorkspaceWindowAssignment> assignments;
  };

  void bindExtWorkspace(ext_workspace_manager_v1* manager);
  void bindKdeVirtualDesktop(org_kde_plasma_virtual_desktop_management* management);
  void bindDwlIpcWorkspace(zdwl_ipc_manager_v2* manager);
  void notifyToplevelsChanged();
  bool syncLabwcMetadata() const;
  void onOutputAdded(wl_output* output);
  void onOutputRemoved(wl_output* output);
  wl_output* resolveOutputName(const std::string& outputName) const;
  std::string connectorNameForOutput(wl_output* output) const;
  std::vector<WorkspaceModelSnapshot> workspaceModelSnapshot() const;
  static bool sameWorkspaceModelSnapshot(
      const std::vector<WorkspaceModelSnapshot>& lhs, const std::vector<WorkspaceModelSnapshot>& rhs
  );
  bool extToplevelIsLive(const ext_foreign_toplevel_handle_v1* handle) const;

  WaylandConnection& m_wayland;
  WorkspaceAlertService* m_workspaceAlertService = nullptr;
  std::unique_ptr<compositors::CompositorRuntimeRegistry> m_runtimeRegistry;
  std::unique_ptr<WaylandWorkspaces> m_workspaces;
  std::unique_ptr<compositors::WorkspaceMetadataBackend> m_workspaceMetadataBackend;
  std::vector<std::unique_ptr<compositors::FocusedOutputBackend>> m_focusedOutputBackends;
  std::unique_ptr<compositors::OutputPowerBackend> m_outputPowerBackend;
  mutable std::optional<bool> m_lastRequestedOutputPowerState;
  bool m_outputPowerOffPending = false;
  std::unique_ptr<KeyboardLayoutBackend> m_keyboardLayoutBackend;
  ChangeCallback m_workspaceChangeCallback;
  ChangeCallback m_overviewChangeCallback;
  ChangeCallback m_keyboardLayoutChangeCallback;
  ChangeCallback m_toplevelChangeCallback;
  std::unique_ptr<compositors::hyprland::HyprlandToplevelMapping> m_hyprlandToplevelMapping;
  std::unique_ptr<compositors::kde::KwinActiveWindow> m_kwinActiveWindow;
  std::unique_ptr<OutputProbe> m_outputProbe;
  std::vector<WorkspaceModelSnapshot> m_lastWorkspaceModelSnapshot;
  std::optional<std::string> m_lastFocusedCompositorWindowId;
  bool m_initialized = false;
};
