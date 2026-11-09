#pragma once

#include "compositors/workspace_backend.h"

#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Zoolander lists workspaces over ext-workspace-v1 and reports each window's
// workspace on its control socket ($XDG_RUNTIME_DIR/zoolander-$WAYLAND_DISPLAY.sock).
// The socket has no event stream, so `clients-json` is polled; each request is
// non-blocking and its socket is serviced through the poll loop.
class ZoolanderWorkspaceBackend final : public compositors::WorkspaceMetadataBackend {
public:
  ZoolanderWorkspaceBackend();
  ~ZoolanderWorkspaceBackend() override;

  ZoolanderWorkspaceBackend(const ZoolanderWorkspaceBackend&) = delete;
  ZoolanderWorkspaceBackend& operator=(const ZoolanderWorkspaceBackend&) = delete;

  void setChangeCallback(ChangeCallback callback) override;
  void setWindowChangeCallback(ChangeCallback callback) override;
  int pollFd() const noexcept override;
  int pollTimeoutMs() const noexcept override;
  void dispatchPoll(short revents) override;
  void apply(std::vector<Workspace>& workspaces, const std::string& outputName = {}) const override;
  std::vector<std::string> workspaceKeys(const std::string& outputName = {}) const override;
  std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(const std::string& outputName = {}) const override;
  std::vector<WorkspaceWindow> workspaceWindows(const std::string& outputName = {}) const override;
  void cleanup() override;

private:
  using Clock = std::chrono::steady_clock;

  struct Client {
    std::string id;
    std::string workspace;
    std::string appId;
    std::string title;
    std::string output;
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool minimized = false;

    bool operator==(const Client&) const = default;
  };

  static std::string workspaceKey(const Workspace& workspace);
  void startRequest();
  void readReply();
  void closeRequest() noexcept;
  void reportFailure(std::string_view reason);
  bool applyReply();

  std::string m_socketPath;
  ChangeCallback m_changeCallback;
  ChangeCallback m_windowChangeCallback;
  std::vector<Client> m_clients;
  std::set<std::string> m_occupied;
  // Workspace keys per output in the order apply() last received them, so
  // workspaceKeys() matches the platform's workspace list.
  mutable std::vector<std::pair<std::string, std::vector<std::string>>> m_keysByOutput;
  int m_fd = -1;
  std::string m_reply;
  Clock::time_point m_nextPoll;
  Clock::time_point m_requestDeadline;
  bool m_loggedFailure = false;
};
