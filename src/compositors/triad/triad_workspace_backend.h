#pragma once

#include "compositors/workspace_backend.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <json.hpp>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace compositors::triad {
  class TriadRuntime;
} // namespace compositors::triad

class TriadWorkspaceBackend final : public WorkspaceBackend,
                                    public compositors::WorkspaceMetadataBackend,
                                    public WorkspaceOutputNameResolver,
                                    public WorkspaceSocketConnector {
public:
  explicit TriadWorkspaceBackend(compositors::triad::TriadRuntime& runtime);
  ~TriadWorkspaceBackend() override;

  TriadWorkspaceBackend(const TriadWorkspaceBackend&) = delete;
  TriadWorkspaceBackend& operator=(const TriadWorkspaceBackend&) = delete;

  [[nodiscard]] const char* backendName() const override { return "triad"; }
  [[nodiscard]] bool isAvailable() const noexcept override;
  void setChangeCallback(WorkspaceBackend::ChangeCallback callback) override;
  void setOverviewChangeCallback(compositors::WorkspaceMetadataBackend::ChangeCallback callback) override;
  void setOutputNameResolver(Resolver resolver) override;
  [[nodiscard]] bool connectSocket() override;
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;
  [[nodiscard]] std::vector<Workspace> all() const override;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const override;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(wl_output* output) const override;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(const std::string& outputName = {}) const override;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(wl_output* output) const override;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(const std::string& outputName = {}) const override;
  void focusWindow(const std::string& windowId) override;
  void cleanup() override;

  [[nodiscard]] int pollFd() const noexcept override { return m_socketFd; }
  [[nodiscard]] short pollEvents() const noexcept override { return POLLIN | POLLHUP | POLLERR; }
  [[nodiscard]] int pollTimeoutMs() const noexcept override;
  void dispatchPoll(short revents) override;
  void apply(std::vector<Workspace>& workspaces, const std::string& outputName = {}) const override;
  [[nodiscard]] std::vector<std::string> workspaceKeys(const std::string& outputName = {}) const override;
  [[nodiscard]] bool canTrackOverviewState() const noexcept override;
  [[nodiscard]] bool hasOverviewState() const noexcept override { return m_overviewKnown; }
  [[nodiscard]] bool isOverviewOpen() const noexcept override { return m_overviewOpen; }
  [[nodiscard]] std::optional<std::string> focusedWindowId() const;

private:
  struct WorkspaceState {
    std::uint32_t index = 0;
    std::uint64_t tagId = 0;
    std::string name;
    std::string output;
    bool active = false;
    bool globalActive = false;
    bool urgent = false;
    bool occupied = false;
    std::optional<std::uint64_t> focusedWindowId;
  };

  struct WindowState {
    std::uint64_t id = 0;
    std::uint32_t workspaceIndex = 0;
    std::string output;
    std::string appId;
    std::string title;
    std::int32_t x = 0;
    std::int32_t y = 0;
  };

  void closeSocket(bool scheduleReconnect);
  void scheduleReconnect();
  void readSocket();
  void parseMessages();
  [[nodiscard]] bool handleMessage(std::string_view line);
  [[nodiscard]] bool applyTriadState(const nlohmann::json& state);
  [[nodiscard]] bool applyOutputs(const nlohmann::json& outputs);
  [[nodiscard]] bool applyLayoutState(const nlohmann::json& state);
  [[nodiscard]] bool applyWindows(const nlohmann::json& windows);
  [[nodiscard]] bool applyWindow(const nlohmann::json& window);
  [[nodiscard]] static std::optional<WorkspaceState> parseWorkspace(const nlohmann::json& json);
  [[nodiscard]] static std::optional<WindowState> parseWindow(const nlohmann::json& json);
  [[nodiscard]] static std::string workspaceKey(const WorkspaceState& workspace);
  [[nodiscard]] static bool isSyntheticPlaceholder(const WorkspaceState& workspace);
  [[nodiscard]] static std::optional<std::uint64_t> jsonUnsigned(const nlohmann::json& json);
  [[nodiscard]] static std::string jsonString(const nlohmann::json& json, const char* key);
  [[nodiscard]] static bool jsonBool(const nlohmann::json& json, const char* key);
  [[nodiscard]] std::string outputNameFor(wl_output* output) const;
  [[nodiscard]] bool shouldExposeWorkspace(const WorkspaceState& workspace, const std::string& outputName = {}) const;
  [[nodiscard]] std::vector<const WorkspaceState*> sortedWorkspaces(const std::string& outputName = {}) const;
  [[nodiscard]] std::optional<std::uint32_t> parseWorkspaceIndex(const std::string& id) const;
  [[nodiscard]] static std::optional<std::uint64_t> parseUnsignedId(const std::string& id);
  void refreshSnapshot();
  void notifyChanged() const;
  void notifyOverviewChanged() const;

  compositors::triad::TriadRuntime& m_runtime;
  int m_socketFd = -1;
  std::vector<char> m_readBuffer;
  std::unordered_set<std::string> m_outputNames;
  std::unordered_map<std::uint32_t, WorkspaceState> m_workspaces;
  std::unordered_map<std::uint64_t, WindowState> m_windows;
  bool m_overviewKnown = false;
  bool m_overviewOpen = false;
  std::chrono::steady_clock::time_point m_nextReconnectAt;
  std::chrono::seconds m_reconnectBackoff{2};
  WorkspaceBackend::ChangeCallback m_changeCallback;
  compositors::WorkspaceMetadataBackend::ChangeCallback m_overviewChangeCallback;
  Resolver m_outputNameResolver;
};
