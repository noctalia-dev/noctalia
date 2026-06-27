#pragma once

#include "compositors/workspace_backend.h"
#include "wayland/wayland_toplevels.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class LabwcWorkspaceBackend final : public compositors::WorkspaceMetadataBackend {
public:
  using WorkspacesProvider = std::function<std::vector<Workspace>()>;
  using ToplevelsProvider = std::function<void(const std::function<void(const WlrToplevelSnapshot&)>&)>;

  void setProviders(WorkspacesProvider workspaces, ToplevelsProvider toplevels);
  [[nodiscard]] bool sync();

  void setChangeCallback(ChangeCallback callback) override;
  void apply(std::vector<Workspace>& workspaces, const std::string& outputName = {}) const override;
  [[nodiscard]] std::vector<std::string> workspaceKeys(const std::string& outputName = {}) const override;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(const std::string& outputName = {}) const override;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(const std::string& outputName = {}) const override;
  void cleanup() override;

private:
  struct TrackedWindow {
    std::string workspaceKey;
    std::string appId;
    std::string title;
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const TrackedWindow&) const = default;
  };

  [[nodiscard]] static std::string workspaceKeyFor(const Workspace& workspace, std::size_t index);
  [[nodiscard]] std::string activeWorkspaceKey(const std::vector<Workspace>& workspaces) const;

  WorkspacesProvider m_workspacesProvider;
  ToplevelsProvider m_toplevelsProvider;
  ChangeCallback m_changeCallback;
  std::unordered_map<std::uintptr_t, TrackedWindow> m_windows;
};