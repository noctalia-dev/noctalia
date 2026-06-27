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
  void setOutputNameResolver(std::function<std::string(wl_output*)> resolver);
  void initialize();
  void onOutputAdded(wl_output* output);
  void onOutputRemoved(wl_output* output);
  void setChangeCallback(ChangeCallback callback);
  void activate(const std::string& id);
  void activateForOutput(wl_output* output, const std::string& id);
  void activateForOutput(wl_output* output, const Workspace& workspace);
  void cleanup();
  [[nodiscard]] int pollFd() const noexcept;
  [[nodiscard]] short pollEvents() const noexcept;
  [[nodiscard]] int pollTimeoutMs() const noexcept;
  void dispatchPoll(short revents);
  [[nodiscard]] const char* backendName() const noexcept;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>> appIdsByWorkspace(wl_output* output) const;
  [[nodiscard]] TaskbarAssignmentMode taskbarAssignmentMode() const noexcept;
  [[nodiscard]] std::unordered_map<std::uintptr_t, WorkspaceWindow>
  assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& windows, wl_output* output) const;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(wl_output* output) const;
  void focusWindow(const std::string& windowId) const;

  [[nodiscard]] std::vector<Workspace> all() const;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const;

  [[nodiscard]] wl_output* mangoIpcSelectedOutput() const;
  [[nodiscard]] std::optional<std::pair<std::string, std::string>>
  mangoIpcFocusedClientOnOutput(wl_output* output) const;
  [[nodiscard]] wl_output* dwlIpcSelectedOutput() const;
  [[nodiscard]] std::optional<std::pair<std::string, std::string>> dwlIpcFocusedClientOnOutput(wl_output* output) const;
  [[nodiscard]] std::optional<std::string> focusedWindowId() const;

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
  WorkspaceBackend* m_triadBackend = nullptr;
  WorkspaceSocketConnector* m_hyprlandConnector = nullptr;
  WorkspaceSocketConnector* m_swayConnector = nullptr;
  WorkspaceSocketConnector* m_triadConnector = nullptr;
  WorkspaceBackend* m_activeBackend = nullptr;
  ChangeCallback m_changeCallback;
};
