#pragma once

#include "compositors/workspace_backend.h"
#include "hyprland_event_handler.h"

#include <cstdint>
#include <functional>
#include <nlohmann/json_fwd.hpp>
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
  // FIXME: remove once Hyprland emits change_id on socket2
  // (https://github.com/hyprwm/Hyprland/discussions/15527).
  void reconcileFromCompositor();

  [[nodiscard]] int pollFd() const noexcept override;
  void dispatchPoll(short revents) override;

private:
  enum class IpcSchema {
    Unknown,
    // TODO: Remove LegacyId after Noctalia drops support for Hyprland v0.56.2 and older.
    LegacyId,
    StableIdentity,
  };

  enum class WorkspaceKind {
    Numbered,
    Named,
    Special,
  };

  enum class WorkspaceRefreshResult {
    Invalid,
    Unchanged,
    Changed,
  };

  struct WorkspaceIdentity {
    std::string key;
    std::string selector;
    std::string address;
    WorkspaceKind kind = WorkspaceKind::Named;
    std::optional<std::uint32_t> number;
    std::optional<int> legacyId;
  };

  struct WorkspaceState {
    WorkspaceIdentity identity;
    std::string name;
    std::string monitor;
    bool active = false;
    bool urgent = false;
    bool occupied = false;
    std::size_t ordinal = 0;
  };

  struct ToplevelState {
    std::string workspaceKey;
    std::string appId;
    std::string title;
    bool urgent = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
  };

  void refreshSnapshot();
  [[nodiscard]] WorkspaceRefreshResult refreshWorkspaces();
  void refreshMonitors();
  void refreshClients();
  void recomputeWorkspaceFlags();
  void ensureSnapshotFresh() const;
  void applyWorkspaceIdChange(std::uint32_t oldId, std::uint32_t newId, std::string_view newName);

  void handleEvent(std::string_view event, std::string_view data) override;
  void handleFocusedMonitor(std::string_view monitorName, std::string_view workspaceKey);
  void handleWorkspaceActivated(std::string_view workspaceKey);
  void clearUrgentForWorkspace(std::string_view workspaceKey);
  void moveToplevel(std::uint64_t address, std::string_view workspaceKey);

  [[nodiscard]] WorkspaceState* findWorkspaceByKey(std::string_view key);
  [[nodiscard]] WorkspaceState* findWorkspaceByName(std::string_view name);
  [[nodiscard]] static std::optional<IpcSchema> detectIpcSchema(const nlohmann::json& workspaces);
  [[nodiscard]] static std::optional<WorkspaceIdentity>
  parseJsonWorkspaceIdentity(const nlohmann::json& json, IpcSchema schema);
  [[nodiscard]] std::optional<WorkspaceIdentity>
  parseEventWorkspaceIdentity(std::string_view selector, std::string_view displayName = {}) const;
  [[nodiscard]] static std::optional<std::uint64_t> parseHexAddress(std::string_view value);
  [[nodiscard]] static std::optional<int> parseInt(std::string_view value);
  [[nodiscard]] static std::optional<std::uint32_t> parseUnsigned(std::string_view value);
  [[nodiscard]] static std::vector<std::string_view> parseEventArgs(std::string_view data, std::size_t count);
  [[nodiscard]] static bool isSpecial(const WorkspaceState& state);
  [[nodiscard]] static bool workspaceOrderLess(const WorkspaceState* a, const WorkspaceState* b);
  [[nodiscard]] static Workspace toWorkspace(const WorkspaceState& state);
  [[nodiscard]] std::string assignmentKeyFor(std::string_view workspaceKey) const;

  OutputNameResolver m_outputNameResolver;
  std::vector<WorkspaceState> m_workspaces;
  std::unordered_map<std::uint64_t, ToplevelState> m_toplevels;
  std::unordered_map<std::string, std::string> m_activeWorkspaceByMonitor;
  std::string m_focusedWindowId;
  std::size_t m_nextOrdinal = 0;
  IpcSchema m_ipcSchema = IpcSchema::Unknown;
  ChangeCallback m_changeCallback;
};
