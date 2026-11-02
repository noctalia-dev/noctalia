#pragma once

#include "compositors/workspace_backend.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

struct wl_output;
struct ext_workspace_manager_v1;
struct org_kde_plasma_virtual_desktop_management;
struct zdwl_ipc_manager_v2;

namespace compositors {
  class CompositorRuntimeRegistry;
} // namespace compositors

class WaylandWorkspaces {
public:
  using ChangeCallback = std::function<void()>;

  explicit WaylandWorkspaces(compositors::CompositorRuntimeRegistry& runtimeRegistry);
  ~WaylandWorkspaces();

  void bindExtWorkspace(ext_workspace_manager_v1* manager);
  void bindKdeVirtualDesktop(org_kde_plasma_virtual_desktop_management* management);
  void bindDwlIpcWorkspace(zdwl_ipc_manager_v2* manager);
  void setOutputNameResolver(const std::function<std::string(wl_output*)>& resolver);
  void initialize();
  void onOutputAdded(wl_output* output);
  void onOutputRemoved(wl_output* output);
  void setChangeCallback(ChangeCallback callback);
  void activate(const std::string& id);
  void activateForOutput(wl_output* output, const std::string& id);
  void activateForOutput(wl_output* output, const Workspace& workspace);
  void cleanup();
  int pollFd() const noexcept;
  short pollEvents() const noexcept;
  int pollTimeoutMs() const noexcept;
  void dispatchPoll(short revents);
  const char* backendName() const noexcept;
  std::unordered_map<std::string, std::vector<std::string>> appIdsByWorkspace(wl_output* output) const;
  TaskbarAssignmentMode taskbarAssignmentMode() const noexcept;
  std::unordered_map<std::uintptr_t, WorkspaceWindow>
  assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& windows, wl_output* output) const;
  std::vector<WorkspaceWindow> workspaceWindows(wl_output* output) const;
  std::vector<std::string> openOverlayWorkspaceKeys(wl_output* output) const;
  void focusWindow(const std::string& windowId) const;

  std::vector<Workspace> all() const;
  std::vector<Workspace> forOutput(wl_output* output) const;

  wl_output* mangoIpcSelectedOutput() const;
  std::optional<std::pair<std::string, std::string>> mangoIpcFocusedClientOnOutput(wl_output* output) const;
  wl_output* dwlIpcSelectedOutput() const;
  std::optional<std::pair<std::string, std::string>> dwlIpcFocusedClientOnOutput(wl_output* output) const;
  std::optional<std::string> focusedWindowId() const;
  // Sway's ext-foreign-toplevel identifier for the focused view; see WorkspaceBackend for why
  // this is not the same namespace as focusedWindowId().
  std::optional<std::string> focusedExtToplevelIdentifier() const;
  bool publishesExtToplevelIdentity() const noexcept;

private:
  void setActiveBackend(WorkspaceBackend* backend);
  void notifyChanged() const;

  std::vector<std::unique_ptr<WorkspaceBackend>> m_backends;
  std::vector<class OutputLifecycleObserver*> m_outputObservers;
  std::vector<WorkspaceOutputNameResolver*> m_outputNameResolvers;
  ExtWorkspaceProtocolBinder* m_extWorkspaceBinder = nullptr;
  KdeVirtualDesktopProtocolBinder* m_kdeVirtualDesktopBinder = nullptr;
  DwlIpcWorkspaceProtocolBinder* m_dwlIpcWorkspaceBinder = nullptr;
  WorkspaceBackend* m_extBackend = nullptr;
  WorkspaceBackend* m_mangoIpcBackend = nullptr;
  WorkspaceSocketConnector* m_mangoIpcConnector = nullptr;
  WorkspaceBackend* m_dwlIpcBackend = nullptr;
  WorkspaceBackend* m_hyprlandBackend = nullptr;
  WorkspaceBackend* m_swayBackend = nullptr;
  WorkspaceBackend* m_kwinBackend = nullptr;
  WorkspaceSocketConnector* m_hyprlandConnector = nullptr;
  WorkspaceSocketConnector* m_swayConnector = nullptr;
  WorkspaceBackend* m_activeBackend = nullptr;
  ChangeCallback m_changeCallback;
};
