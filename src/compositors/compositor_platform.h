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

  [[nodiscard]] WaylandConnection& wayland() noexcept { return m_wayland; }
  [[nodiscard]] const WaylandConnection& wayland() const noexcept { return m_wayland; }
  [[nodiscard]] wl_display* display() const noexcept;
  [[nodiscard]] bool hasXdgShell() const noexcept;
  [[nodiscard]] bool hasXdgActivation() const noexcept;
  [[nodiscard]] std::string requestActivationToken(wl_surface* surface) const;
  [[nodiscard]] bool hasGammaControl() const noexcept;
  [[nodiscard]] const std::vector<WaylandOutput>& outputs() const noexcept;
  [[nodiscard]] const WaylandOutput* findOutputByWl(wl_output* output) const;
  [[nodiscard]] wl_output* outputForSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] FocusGrabService* focusGrabService() const noexcept;
  [[nodiscard]] bool hasPointerPosition() const noexcept;
  [[nodiscard]] wl_surface* lastPointerSurface() const noexcept;
  [[nodiscard]] wl_surface* lastKeyboardSurface() const noexcept;
  [[nodiscard]] double lastPointerX() const noexcept;
  [[nodiscard]] double lastPointerY() const noexcept;
  [[nodiscard]] std::uint32_t lastInputSerial() const noexcept;
  [[nodiscard]] zwlr_layer_surface_v1* layerSurfaceFor(wl_surface* surface) const noexcept;
  void stopKeyRepeat();
  void setCursorShape(std::uint32_t serial, std::uint32_t shape);

  // Output resolved from a real focus source (compositor IPC, focused-output
  // backend, active toplevel, keyboard/fresh-pointer surface). Returns nullptr
  // when no such source is available — callers that need an output regardless
  // should use preferredInteractiveOutput() or probeFocusedOutput().
  [[nodiscard]] wl_output*
  focusedInteractiveOutput(std::chrono::milliseconds pointerMaxAge = std::chrono::milliseconds(1200)) const;

  [[nodiscard]] wl_output*
  preferredInteractiveOutput(std::chrono::milliseconds pointerMaxAge = std::chrono::milliseconds(1200)) const;

  // Asks the compositor which output a NULL-output layer surface lands on (the
  // focused one) by mapping a throwaway 1x1 probe surface, then reports it. For
  // compositors with no focus source, where focusedInteractiveOutput() is null.
  // The callback receives the probed output, or nullptr on timeout. Only one
  // probe runs at a time; a new call cancels any in-flight probe.
  void probeFocusedOutput(std::function<void(wl_output*)> callback, std::chrono::milliseconds timeout);

  [[nodiscard]] std::optional<ActiveToplevel> activeToplevel() const;
  [[nodiscard]] wl_output* activeToplevelOutput() const;
  [[nodiscard]] std::vector<std::string> runningAppIds(wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<ToplevelInfo>
  windowsForApp(const std::string& idLower, const std::string& wmClassLower, wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<ToplevelInfo> windowsWithoutAppId(wl_output* outputFilter = nullptr) const;
  [[nodiscard]] bool containsWlrToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) const;
  void activateToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  void activateToplevelInfo(const ToplevelInfo& window);
  void closeToplevel(zwlr_foreign_toplevel_handle_v1* handle);
  void closeToplevelInfo(const ToplevelInfo& window);
  void focusCompositorWindow(const std::string& windowId) const;

  // Focus `output` before spawning (Hyprland) so clients follow the launch monitor.
  void prepareAppLaunchOnOutput(wl_output* output);

  // Move a toplevel onto `output` when supported (Hyprland).
  void moveToplevelToOutput(const ToplevelInfo& window, wl_output* output);

  void activateKdeWindow(const std::string& title, const std::string& appId, const std::string& uuid = {});

  void setToplevelChangeCallback(ChangeCallback callback);
  void bindHyprlandToplevelMappingManager(hyprland_toplevel_mapping_manager_v1* manager);
  void syncHyprlandToplevelMappings();
  [[nodiscard]] std::optional<std::string> compositorWindowIdForToplevel(zwlr_foreign_toplevel_handle_v1* handle) const;
  [[nodiscard]] std::optional<std::string>
  compositorWindowIdForExtToplevel(ext_foreign_toplevel_handle_v1* handle) const;
  [[nodiscard]] std::optional<std::string> compositorWindowIdForToplevelInfo(const ToplevelInfo& info) const;
  [[nodiscard]] zwlr_foreign_toplevel_handle_v1* toplevelHandleForCompositorWindowId(std::string_view windowId) const;
  [[nodiscard]] bool isCompositorWindowIdKnown(std::string_view windowId) const;
  [[nodiscard]] std::optional<std::string> focusedCompositorWindowId() const;

  void setWorkspaceChangeCallback(ChangeCallback callback);
  void setOverviewChangeCallback(ChangeCallback callback);
  void activateWorkspace(const std::string& id);
  void activateWorkspace(wl_output* output, const std::string& id);
  void activateWorkspace(wl_output* output, const Workspace& workspace);
  std::size_t addWorkspacePollFds(std::vector<pollfd>& fds) const;
  [[nodiscard]] int workspacePollTimeoutMs() const noexcept;
  void dispatchWorkspacePoll(const std::vector<pollfd>& fds, std::size_t startIdx);
  [[nodiscard]] std::vector<Workspace> workspaces() const;
  [[nodiscard]] std::vector<Workspace> workspaces(wl_output* output) const;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<std::string> workspaceDisplayKeys(wl_output* outputFilter = nullptr) const;
  [[nodiscard]] std::vector<WorkspaceWindowAssignment>
  workspaceWindowAssignments(wl_output* outputFilter = nullptr) const;

  // Workspace alerts: user-requested "attention" markers overlaid onto the
  // workspace model by reusing Workspace::id (no new per-backend identifier).
  void setWorkspaceAlertService(WorkspaceAlertService* service) noexcept;
  [[nodiscard]] bool isKnownWorkspaceAlertKey(std::string_view workspaceId) const;
  [[nodiscard]] std::optional<std::string> workspaceAlertKeyForWindow(std::string_view windowId) const;
  [[nodiscard]] std::size_t clearActiveWorkspaceAlerts(wl_output* output);
  [[nodiscard]] std::size_t clearActiveWorkspaceAlerts();
  [[nodiscard]] TaskbarAssignmentMode taskbarAssignmentMode() const noexcept;
  [[nodiscard]] bool supportsTaskbarWorkspaceGrouping() const noexcept;
  [[nodiscard]] std::unordered_map<std::uintptr_t, WorkspaceWindow>
  assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& windows, wl_output* outputFilter = nullptr) const;
  [[nodiscard]] const char* workspaceBackendName() const noexcept;

  [[nodiscard]] bool cycleKeyboardLayout() const;
  [[nodiscard]] bool hasKeyboardLayoutBackend() const noexcept;
  [[nodiscard]] std::optional<KeyboardLayoutState> keyboardLayoutState() const;
  [[nodiscard]] std::string currentKeyboardLayoutName() const;
  [[nodiscard]] std::vector<std::string> keyboardLayoutNames() const;

  void setKeyboardLayoutChangeCallback(ChangeCallback callback);
  void addKeyboardLayoutPollFds(std::vector<pollfd>& fds) const;
  void dispatchKeyboardLayoutPoll(const std::vector<pollfd>& fds, std::size_t startIdx);

  [[nodiscard]] bool requestSessionExit() const;
  [[nodiscard]] bool setOutputPower(bool on) const;

  [[nodiscard]] bool tracksOverviewState() const noexcept;
  [[nodiscard]] bool hasOverviewState() const noexcept;
  [[nodiscard]] bool isOverviewOpen() const noexcept;

  [[nodiscard]] compositors::niri::NiriRuntime& niriRuntime() noexcept;
  [[nodiscard]] const compositors::niri::NiriRuntime& niriRuntime() const noexcept;

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
  void onOutputAdded(wl_output* output);
  void onOutputRemoved(wl_output* output);
  [[nodiscard]] wl_output* resolveOutputName(const std::string& outputName) const;
  [[nodiscard]] std::string connectorNameForOutput(wl_output* output) const;
  [[nodiscard]] std::vector<WorkspaceModelSnapshot> workspaceModelSnapshot() const;
  [[nodiscard]] static bool sameWorkspaceModelSnapshot(
      const std::vector<WorkspaceModelSnapshot>& lhs, const std::vector<WorkspaceModelSnapshot>& rhs
  );

  WaylandConnection& m_wayland;
  WorkspaceAlertService* m_workspaceAlertService = nullptr;
  std::unique_ptr<compositors::CompositorRuntimeRegistry> m_runtimeRegistry;
  std::unique_ptr<WaylandWorkspaces> m_workspaces;
  std::unique_ptr<compositors::WorkspaceMetadataBackend> m_workspaceMetadataBackend;
  std::vector<std::unique_ptr<compositors::FocusedOutputBackend>> m_focusedOutputBackends;
  std::unique_ptr<compositors::OutputPowerBackend> m_outputPowerBackend;
  mutable std::optional<bool> m_lastRequestedOutputPowerState;
  std::unique_ptr<KeyboardLayoutBackend> m_keyboardLayoutBackend;
  ChangeCallback m_workspaceChangeCallback;
  ChangeCallback m_overviewChangeCallback;
  ChangeCallback m_keyboardLayoutChangeCallback;
  ChangeCallback m_toplevelChangeCallback;
  std::unique_ptr<compositors::hyprland::HyprlandToplevelMapping> m_hyprlandToplevelMapping;
  std::unique_ptr<compositors::kde::KwinActiveWindow> m_kwinActiveWindow;
  std::unique_ptr<OutputProbe> m_outputProbe;
  std::vector<WorkspaceModelSnapshot> m_lastWorkspaceModelSnapshot;
  bool m_initialized = false;
};
