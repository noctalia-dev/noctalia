#pragma once

#include "compositors/workspace_backend.h"
#include "shell/wayland/wayland_toplevels.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Per-window workspaces for Wayfire. Workspaces themselves come from ext-workspace (one per cell
// of each output's grid); Wayfire's IPC (the ipc and ipc-rules plugins) reports where each window
// sits, from which its grid cell follows the way Wayfire derives it: from the window's centre.
class WayfireWorkspaceBackend final : public compositors::WorkspaceMetadataBackend {
public:
  // Workspaces of one output by connector name, in ext-workspace order; empty name means all.
  using WorkspacesProvider = std::function<std::vector<Workspace>(const std::string& outputName)>;
  using ToplevelsProvider = std::function<void(const std::function<void(const WlrToplevelSnapshot&)>&)>;

  WayfireWorkspaceBackend();
  ~WayfireWorkspaceBackend() override;

  void setProviders(WorkspacesProvider workspaces, ToplevelsProvider toplevels);

  void setChangeCallback(ChangeCallback callback) override;
  int pollFd() const noexcept override { return m_socketFd; }
  void dispatchPoll(short revents) override;
  void apply(std::vector<Workspace>& workspaces, const std::string& outputName = {}) const override;
  std::vector<std::string> workspaceKeys(const std::string& outputName = {}) const override;
  std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(const std::string& outputName = {}) const override;
  std::vector<WorkspaceWindow> workspaceWindows(const std::string& outputName = {}) const override;
  void cleanup() override;

  // Public for tests: feed IPC replies without a socket.
  void parseOutputs(const std::string& payload);
  void parseViews(const std::string& payload);

private:
  struct OutputState {
    double width = 0.0;
    double height = 0.0;
    int workspaceX = 0;
    int workspaceY = 0;
    int gridWidth = 1;
    int gridHeight = 1;
  };

  struct ViewState {
    std::int64_t id = 0;
    std::string appId;
    std::string title;
    std::string outputName;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool sticky = false;
  };

  enum class Reply : std::uint8_t { Watch, Outputs, Views };

  void connectSocket();
  void send(const std::string& method, const std::string& data = "{}");
  void requestRefresh();
  void readSocket();
  void handleMessage(const std::string& payload);
  // Grid cell of a view as (column, row), or false when its output is unknown.
  bool cellForView(const ViewState& view, int& column, int& row) const;
  // Key (name, else id) of the workspace at a grid cell: from the cell in its name when Wayfire's own
  // ext-workspace plugin named it, otherwise from its ext-workspace coordinates as (row, column).
  static std::string workspaceKeyAt(const std::vector<Workspace>& workspaces, int column, int row);

  WorkspacesProvider m_workspacesProvider;
  ToplevelsProvider m_toplevelsProvider;
  ChangeCallback m_changeCallback;

  int m_socketFd = -1;
  std::vector<char> m_readBuffer;
  std::deque<Reply> m_pendingReplies;
  // An event arrived while a refresh was in flight: refresh once more when it lands.
  bool m_refreshQueued = false;

  std::unordered_map<std::string, OutputState> m_outputs;
  std::vector<ViewState> m_views;
};
