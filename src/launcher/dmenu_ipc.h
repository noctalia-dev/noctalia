#pragma once

#include "app/poll_source.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class LauncherPanel;
class PanelManager;

// Dedicated Unix socket implementing the `noctalia dmenu` stdin/stdout contract.
// The main IPC service is synchronous and line-capped, so it cannot carry large
// candidate lists or defer a reply until the user picks. This socket accepts one
// connection per invocation, reads a prompt header line + candidate bytes until EOF,
// opens the launcher scoped to an ephemeral provider, and writes the selection (or
// closes the fd for cancel) back on the same connection. All I/O runs on the poll thread.
class DmenuIpcService final : public PollSource {
public:
  DmenuIpcService();
  ~DmenuIpcService() override;

  // Creates and binds the listening socket. Returns false if it could not be created
  // (dmenu stays unavailable; the shell is otherwise unaffected).
  bool start();

  void setLauncherPanel(LauncherPanel* panel);
  void setPanelManager(PanelManager* panelManager);

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

private:
  struct Session {
    std::size_t token = 0;
    int connFd = -1;
    std::string buffer;    // accumulating bytes: prompt line then candidate data
    bool receiving = true; // still reading candidate bytes (polled); false once EOF seen
    bool sending = false;  // response bytes are pending (polled for POLLOUT)
    std::string response;
    std::size_t responseSent = 0;
    std::string providerId; // set when the ephemeral provider has been added
  };

  void acceptConnections();
  void drainSession(Session& session);
  void flushSessionResponse(Session& session);
  void finalizeSession(Session& session);
  // Write selection (if any) to the client, close the fd, erase the session, and defer
  // panel cleanup. Used for sessions that have an active provider.
  void completeSession(int connFd, std::optional<std::string> selection);
  // Close the fd and erase the session without touching the launcher (empty disconnect
  // or a rejected request that never opened a provider).
  void discardSession(int connFd);

  int m_listenFd = -1;
  std::string m_socketPath;
  LauncherPanel* m_launcherPanel = nullptr;
  PanelManager* m_panelManager = nullptr;
  std::size_t m_nextToken = 1;
  std::vector<std::unique_ptr<Session>> m_sessions;
};
