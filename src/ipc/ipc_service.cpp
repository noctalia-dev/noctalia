#include "ipc/ipc_service.h"

#include "core/log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

  constexpr Logger kLog("ipc");
  constexpr int kMaxLineBytes = 512;
  constexpr int kRecvTimeoutMs = 100;

} // namespace

IpcService::~IpcService() {
  if (m_listenFd >= 0) {
    ::close(m_listenFd);
    m_listenFd = -1;
  }
  if (!m_socketPath.empty()) {
    ::unlink(m_socketPath.c_str());
  }
}

bool IpcService::start() {
  m_socketPath = resolveSocketPath();
  if (m_socketPath.empty()) {
    kLog.warn("IPC disabled: could not determine socket path");
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    kLog.warn("IPC disabled: socket() failed: {}", std::strerror(errno));
    return false;
  }

  // Remove stale socket file before binding
  ::unlink(m_socketPath.c_str());

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (m_socketPath.size() >= sizeof(addr.sun_path)) {
    kLog.warn("IPC disabled: socket path too long");
    ::close(fd);
    return false;
  }
  std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    kLog.warn("IPC disabled: bind() failed: {}", std::strerror(errno));
    ::close(fd);
    return false;
  }

  if (::listen(fd, 128) < 0) {
    kLog.warn("IPC disabled: listen() failed: {}", std::strerror(errno));
    ::close(fd);
    ::unlink(m_socketPath.c_str());
    return false;
  }

  m_listenFd = fd;
  return true;
}

void IpcService::registerHandler(
    const std::string& command, Handler handler, std::string usage, std::string description
) {
  // Remove existing entry for this command if re-registering
  m_handlers.erase(
      std::remove_if(m_handlers.begin(), m_handlers.end(), [&command](const auto& e) { return e.first == command; }),
      m_handlers.end()
  );
  m_handlers.push_back({command, {std::move(handler), std::move(usage), std::move(description)}});
}

void IpcService::dispatch() {
  while (true) {
    const int connFd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (connFd < 0) {
      break; // EAGAIN / EWOULDBLOCK — no more pending connections
    }
    handleConnection(connFd);
    ::close(connFd);
  }
}

std::string IpcService::execute(const std::string& line) const {
  std::string command;
  std::string args;
  const auto spacePos = line.find(' ');
  if (spacePos == std::string::npos) {
    command = line;
  } else {
    command = line.substr(0, spacePos);
    args = line.substr(spacePos + 1);
  }

  if (command == "--help" || command == "-h") {
    return buildHelp();
  }

  return executeParsed(command, args);
}

void IpcService::handleConnection(int connFd) {
  // Set receive timeout so a slow client doesn't stall the main loop
  timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = kRecvTimeoutMs * 1000;
  ::setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Read up to kMaxLineBytes or until '\n'
  char buf[kMaxLineBytes];
  int total = 0;
  while (total < kMaxLineBytes - 1) {
    const auto n = ::read(connFd, buf + total, 1);
    if (n <= 0) {
      break;
    }
    if (buf[total] == '\n') {
      break;
    }
    ++total;
  }
  buf[total] = '\0';

  // Trim trailing '\r' for clients that send CRLF
  if (total > 0 && buf[total - 1] == '\r') {
    buf[--total] = '\0';
  }

  if (total == 0) {
    // Client closed the connection without sending anything (e.g. a liveness probe).
    return;
  }

  const std::string response = execute(std::string(buf, static_cast<std::size_t>(total)));
  std::size_t sent = 0;
  while (sent < response.size()) {
    const auto n = ::send(connFd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      break;
    }
    sent += static_cast<std::size_t>(n);
  }
}

std::string IpcService::buildHelp() const {
  std::vector<std::size_t> order(m_handlers.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [this](std::size_t lhs, std::size_t rhs) {
    return m_handlers[lhs].first < m_handlers[rhs].first;
  });

  // Find the longest usage string for alignment
  std::size_t maxUsage = 0;
  for (const auto& [cmd, entry] : m_handlers) {
    const auto& u = entry.usage.empty() ? cmd : entry.usage;
    maxUsage = std::max(maxUsage, u.size());
  }

  std::string out = "Usage: noctalia msg <command> [args]\n\nCommands:\n";
  for (const auto index : order) {
    const auto& [cmd, entry] = m_handlers[index];
    const auto& u = entry.usage.empty() ? cmd : entry.usage;
    out += "  ";
    out += u;
    if (!entry.description.empty()) {
      out += std::string(maxUsage - u.size() + 2, ' ');
      out += entry.description;
    }
    out += '\n';
  }
  return out;
}

std::string IpcService::executeParsed(const std::string& command, const std::string& args) const {
  const auto it =
      std::find_if(m_handlers.begin(), m_handlers.end(), [&command](const auto& e) { return e.first == command; });
  if (it == m_handlers.end()) {
    return "error: unknown command (try: noctalia msg --help)\n";
  }
  return it->second.fn(args);
}

std::string IpcService::resolveSocketPath() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || runtime[0] == '\0') {
    runtime = "/tmp";
  }
  const char* display = std::getenv("WAYLAND_DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    display = "wayland-0";
  }
  return std::string(runtime) + "/noctalia-" + display + ".sock";
}
