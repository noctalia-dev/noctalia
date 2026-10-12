#pragma once

#include "compositors/workspace_backend.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace compositors::sway {
  class SwayRuntime;
} // namespace compositors::sway

class SwayWorkspaceBackend final : public WorkspaceBackend,
                                   public WorkspaceOutputNameResolver,
                                   public WorkspaceSocketConnector {
public:
  using OutputNameResolver = WorkspaceOutputNameResolver::Resolver;

  SwayWorkspaceBackend(OutputNameResolver outputNameResolver, compositors::sway::SwayRuntime& runtime);

  bool connectSocket() override;
  void setOutputNameResolver(OutputNameResolver outputNameResolver) override;

  [[nodiscard]] const char* backendName() const override { return "sway-ipc"; }
  [[nodiscard]] bool isAvailable() const noexcept override { return m_socketFd >= 0; }
  void setChangeCallback(ChangeCallback callback) override;
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;
  [[nodiscard]] std::vector<Workspace> all() const override;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const override;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(wl_output* output) const override;
  [[nodiscard]] TaskbarAssignmentMode taskbarAssignmentMode() const noexcept override {
    return TaskbarAssignmentMode::WorkspaceOccurrenceTitle;
  }
  [[nodiscard]] std::unordered_map<std::uintptr_t, WorkspaceWindow>
  assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& windows, wl_output* output) const override;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(wl_output* output) const override;
  void focusWindow(const std::string& windowId) override;
  void cleanup() override;

  [[nodiscard]] int pollFd() const noexcept override { return m_socketFd; }
  void dispatchPoll(short revents) override;

private:
  struct SwayWorkspace {
    std::string name;
    std::string output;
    bool visible = false;
    bool urgent = false;
    bool occupied = false;
    int num = -1;
    std::size_t ordinal = 0;
  };

  void requestSnapshot();
  void sendMessage(std::uint32_t type, const std::string& payload);
  void readSocket();
  void parseMessages();
  void handleMessage(std::uint32_t type, const std::string& payload);
  void parseWorkspaceList(const std::string& payload);
  void parseTree(const std::string& payload);
  void requestTree();
  void refreshFromWorkspaceEvent();
  [[nodiscard]] static Workspace toWorkspace(const SwayWorkspace& workspace);

  OutputNameResolver m_outputNameResolver;
  compositors::sway::SwayRuntime& m_runtime;
  int m_socketFd = -1;
  std::vector<char> m_readBuffer;
  std::vector<SwayWorkspace> m_workspaces;
  std::unordered_map<std::string, std::size_t> m_workspaceOccupancy;
  std::unordered_map<std::string, std::vector<std::string>> m_workspaceApps;
  std::vector<WorkspaceWindow> m_workspaceWindows;
  ChangeCallback m_changeCallback;
};
