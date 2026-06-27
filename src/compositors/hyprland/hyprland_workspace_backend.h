#pragma once

#include "compositors/workspace_backend.h"
#include "hyprland_event_handler.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace compositors::hyprland {
  class HyprlandRuntime;
  class HyprlandEventHandler;
} // namespace compositors::hyprland

class HyprlandWorkspaceBackend final : public WorkspaceBackend,
                                       public WorkspaceOutputNameResolver,
                                       public WorkspaceSocketConnector,
                                       public compositors::hyprland::HyprlandEventHandler {
public:
  using OutputNameResolver = WorkspaceOutputNameResolver::Resolver;

  HyprlandWorkspaceBackend(OutputNameResolver outputNameResolver, compositors::hyprland::HyprlandRuntime& runtime);

  bool connectSocket() override;
  void setOutputNameResolver(OutputNameResolver outputNameResolver) override;

  [[nodiscard]] const char* backendName() const override { return "hyprland-ipc"; }
  [[nodiscard]] bool isAvailable() const noexcept override;
  void setChangeCallback(ChangeCallback callback) override;
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;
  [[nodiscard]] std::vector<Workspace> all() const override;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const override;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(wl_output* output) const override;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(wl_output* output) const override;
  [[nodiscard]] std::optional<std::string> focusedWindowId() const;
  void focusWindow(const std::string& windowId) override;
  void cleanup() override;
  void notifyCleanup() override;
  void notifyChanged() override;
  void syncFromCompositor();

  [[nodiscard]] int pollFd() const noexcept override;
  void dispatchPoll(short revents) override;

private:
  struct WorkspaceState {
    int id = -1;
    std::string name;
    std::string monitor;
    bool active = false;
    bool urgent = false;
    bool occupied = false;
    std::size_t ordinal = 0;
  };

  struct ToplevelState {
    int workspaceId;
    std::string appId;
    std::string title;
    bool urgent = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
  };

  void refreshSnapshot();
  void refreshWorkspaces();
  void refreshMonitors();
  void refreshClients();
  void recomputeWorkspaceFlags();
  void ensureSnapshotFresh() const;

  void handleEvent(std::string_view event, std::string_view data) override;
  void handleFocusedMonitor(std::string_view monitorName, int workspaceId);
  void handleWorkspaceActivated(int workspaceId);
  void clearUrgentForWorkspace(int workspaceId);
  void moveToplevel(std::uint64_t address, int workspaceId);

  [[nodiscard]] WorkspaceState* findWorkspaceById(int id);
  [[nodiscard]] WorkspaceState* findWorkspaceByName(std::string_view name);
  [[nodiscard]] static std::optional<std::uint64_t> parseHexAddress(std::string_view value);
  [[nodiscard]] static std::optional<int> parseInt(std::string_view value);
  [[nodiscard]] static std::vector<std::string_view> parseEventArgs(std::string_view data, std::size_t count);
  [[nodiscard]] static bool isSpecial(const WorkspaceState& state);
  [[nodiscard]] static bool workspaceOrderLess(const WorkspaceState* a, const WorkspaceState* b);
  [[nodiscard]] static Workspace toWorkspace(const WorkspaceState& state);
  [[nodiscard]] std::string workspaceKeyForId(int workspaceId) const;

  OutputNameResolver m_outputNameResolver;
  std::vector<WorkspaceState> m_workspaces;
  std::unordered_map<std::uint64_t, ToplevelState> m_toplevels;
  std::unordered_map<std::string, int> m_activeWorkspaceByMonitor;
  std::string m_focusedWindowId;
  std::size_t m_nextOrdinal = 0;
  ChangeCallback m_changeCallback;
};
