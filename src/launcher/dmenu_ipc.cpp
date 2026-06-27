#include "launcher/dmenu_ipc.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "launcher/dmenu_socket_path.h"
#include "launcher/dmenu_stdin_provider.h"
#include "shell/launcher/launcher_panel.h"
#include "shell/panel/panel_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("dmenu");
  constexpr std::size_t kRecvChunk = 8192;

} // namespace

DmenuIpcService::DmenuIpcService() = default;

DmenuIpcService::~DmenuIpcService() {
  for (auto& session : m_sessions) {
    if (session->connFd >= 0) {
      ::close(session->connFd);
    }
  }
  if (m_listenFd >= 0) {
    ::close(m_listenFd);
  }
  if (!m_socketPath.empty()) {
    ::unlink(m_socketPath.c_str());
  }
}

bool DmenuIpcService::start() {
  const auto socketPath = noctalia::launcher::resolveDmenuSocketPath();
  m_socketPath = socketPath.path;
  if (m_socketPath.empty()) {
    kLog.warn("disabled: {}", socketPath.error);
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    kLog.warn("disabled: socket() failed: {}", std::strerror(errno));
    return false;
  }

  ::unlink(m_socketPath.c_str());

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (m_socketPath.size() >= sizeof(addr.sun_path)) {
    kLog.warn("disabled: socket path too long");
    ::close(fd);
    return false;
  }
  std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    kLog.warn("disabled: bind() failed: {}", std::strerror(errno));
    ::close(fd);
    return false;
  }
  if (::listen(fd, 8) < 0) {
    kLog.warn("disabled: listen() failed: {}", std::strerror(errno));
    ::close(fd);
    ::unlink(m_socketPath.c_str());
    return false;
  }

  m_listenFd = fd;
  kLog.info("listening on {}", m_socketPath);
  return true;
}

void DmenuIpcService::setLauncherPanel(LauncherPanel* panel) { m_launcherPanel = panel; }

void DmenuIpcService::setPanelManager(PanelManager* panelManager) { m_panelManager = panelManager; }

void DmenuIpcService::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_listenFd >= 0) {
    fds.push_back({.fd = m_listenFd, .events = POLLIN, .revents = 0});
  }
  for (auto& session : m_sessions) {
    if (session->receiving) {
      fds.push_back({.fd = session->connFd, .events = POLLIN, .revents = 0});
    } else if (session->sending) {
      fds.push_back({.fd = session->connFd, .events = POLLOUT, .revents = 0});
    }
  }
}

void DmenuIpcService::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  std::size_t idx = startIdx;
  if (m_listenFd >= 0 && idx < fds.size() && (fds[idx].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
    acceptConnections();
    ++idx;
  } else if (m_listenFd >= 0) {
    ++idx;
  }

  // Session fds were added in m_sessions order after the listen fd.
  std::vector<int> readyReadFds;
  std::vector<int> readyWriteFds;
  for (auto& session : m_sessions) {
    if (!session->receiving && !session->sending) {
      continue;
    }
    if (idx < fds.size() && fds[idx].revents != 0) {
      if (session->receiving) {
        readyReadFds.push_back(session->connFd);
      } else {
        readyWriteFds.push_back(session->connFd);
      }
    }
    ++idx;
  }

  for (const int connFd : readyReadFds) {
    const auto it = std::ranges::find_if(m_sessions, [connFd](const std::unique_ptr<Session>& session) {
      return session->connFd == connFd && session->receiving;
    });
    if (it != m_sessions.end()) {
      drainSession(**it);
    }
  }

  for (const int connFd : readyWriteFds) {
    const auto it = std::ranges::find_if(m_sessions, [connFd](const std::unique_ptr<Session>& session) {
      return session->connFd == connFd && session->sending;
    });
    if (it != m_sessions.end()) {
      flushSessionResponse(**it);
    }
  }
}

void DmenuIpcService::acceptConnections() {
  while (true) {
    const int connFd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connFd < 0) {
      break;
    }
    auto session = std::make_unique<Session>();
    session->connFd = connFd;
    session->token = m_nextToken++;
    m_sessions.push_back(std::move(session));
  }
}

void DmenuIpcService::drainSession(Session& session) {
  char buf[kRecvChunk];
  for (;;) {
    const auto n = ::read(session.connFd, buf, sizeof(buf));
    if (n > 0) {
      session.buffer.append(buf, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    // EOF (n == 0) or error: end of input.
    session.receiving = false;
    if (session.buffer.empty()) {
      discardSession(session.connFd);
    } else {
      finalizeSession(session);
    }
    return;
  }
}

void DmenuIpcService::flushSessionResponse(Session& session) {
  while (session.responseSent < session.response.size()) {
    const auto n = ::send(
        session.connFd, session.response.data() + session.responseSent, session.response.size() - session.responseSent,
        MSG_NOSIGNAL
    );
    if (n > 0) {
      session.responseSent += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    discardSession(session.connFd);
    return;
  }

  discardSession(session.connFd);
}

void DmenuIpcService::finalizeSession(Session& session) {
  if (m_launcherPanel == nullptr || m_panelManager == nullptr) {
    discardSession(session.connFd);
    return;
  }
  // Only one dmenu session can drive the launcher at a time; don't disrupt an open one.
  if (m_panelManager->isOpenPanel("launcher")) {
    kLog.debug("rejecting session {}: launcher already open", session.token);
    discardSession(session.connFd);
    return;
  }

  const std::string& buf = session.buffer;
  std::vector<std::string> lines;
  std::string prompt;
  std::size_t begin;
  const auto nl = buf.find('\n');
  if (nl == std::string::npos) {
    // No header newline: treat the whole payload as candidates.
    begin = 0;
  } else {
    prompt = buf.substr(0, nl);
    if (!prompt.empty() && prompt.back() == '\r') {
      prompt.pop_back();
    }
    begin = nl + 1;
  }
  for (std::size_t i = begin; i <= buf.size(); ++i) {
    if (i < buf.size() && buf[i] != '\n') {
      continue;
    }
    std::size_t end = i;
    if (end > begin && buf[end - 1] == '\r') {
      --end;
    }
    if (end > begin) {
      lines.emplace_back(buf.substr(begin, end - begin));
    }
    begin = i + 1;
  }

  const std::string providerId = "dmenu.stdin." + std::to_string(session.token);
  session.providerId = providerId;
  const int connFd = session.connFd;

  auto provider = std::make_unique<DmenuStdinProvider>(
      std::move(lines), providerId,
      [this, connFd](std::optional<std::string> selection) { completeSession(connFd, std::move(selection)); }
  );
  m_launcherPanel->addProvider(std::move(provider));
  m_launcherPanel->setScopedProvider(providerId, prompt);
  m_panelManager->openPanel("launcher", PanelOpenRequest{});
}

void DmenuIpcService::completeSession(int connFd, std::optional<std::string> selection) {
  const auto it =
      std::ranges::find_if(m_sessions, [connFd](const std::unique_ptr<Session>& s) { return s->connFd == connFd; });
  if (it == m_sessions.end()) {
    return;
  }

  if (selection.has_value()) {
    (*it)->response = *selection;
    (*it)->response += '\n';
    (*it)->responseSent = 0;
    (*it)->sending = true;
    flushSessionResponse(**it);
  } else {
    discardSession(connFd);
  }

  // The provider is being iterated by LauncherPanel during activate()/reset(); mutate
  // the panel after this event completes.
  DeferredCall::callLater([this]() {
    if (m_launcherPanel != nullptr) {
      m_launcherPanel->clearProvidersWithIdPrefix("dmenu.stdin.");
      m_launcherPanel->setScopedProvider({});
    }
  });
}

void DmenuIpcService::discardSession(int connFd) {
  const auto it =
      std::ranges::find_if(m_sessions, [connFd](const std::unique_ptr<Session>& s) { return s->connFd == connFd; });
  if (it == m_sessions.end()) {
    return;
  }
  ::close(connFd);
  m_sessions.erase(it);
}
