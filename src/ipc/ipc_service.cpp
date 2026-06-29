#include "ipc/ipc_service.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

  constexpr Logger kLog("ipc");
  constexpr std::size_t kMaxCommandBytes = 64 * 1024;
  constexpr int kRecvTimeoutMs = 100;
  constexpr char kCallerCwdSeparator = '\x1e';

  [[nodiscard]] std::optional<std::string> parseCallerCwdPrefix(std::string& line) {
    const auto separator = line.find(kCallerCwdSeparator);
    if (separator == std::string::npos) {
      return std::nullopt;
    }

    std::string cwd = line.substr(0, separator);
    line.erase(0, separator + 1);
    if (cwd.empty()) {
      return std::nullopt;
    }

    std::error_code ec;
    const std::filesystem::path cwdPath(cwd);
    if (!cwdPath.is_absolute() || !std::filesystem::is_directory(cwdPath, ec) || ec) {
      return std::nullopt;
    }
    return cwd;
  }

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
    const std::string& command, Handler handler, std::string usage, std::string description,
    HandlerVisibility visibility
) {
  // Remove existing entry for this command if re-registering
  std::erase_if(m_handlers, [&command](const auto& e) { return e.first == command; });
  m_handlers.push_back({command, {std::move(handler), std::move(usage), std::move(description), visibility}});
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
  auto* self = const_cast<IpcService*>(this);
  self->m_callerCwd.reset();

  std::string commandLine = line;
  if (auto cwd = parseCallerCwdPrefix(commandLine); cwd.has_value()) {
    self->m_callerCwd = std::move(*cwd);
  }

  std::string command;
  std::string args;
  const auto spacePos = commandLine.find(' ');
  if (spacePos == std::string::npos) {
    command = commandLine;
  } else {
    command = commandLine.substr(0, spacePos);
    args = commandLine.substr(spacePos + 1);
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

  // Read until the client closes its write side. Newlines are valid command
  // payload, so they cannot be used as the frame delimiter.
  std::string command;
  char buf[4096];
  bool reachedEof = false;
  while (command.size() < kMaxCommandBytes) {
    const std::size_t limit = std::min(sizeof(buf), kMaxCommandBytes - command.size());
    const auto n = ::read(connFd, buf, limit);
    if (n > 0) {
      command.append(buf, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      reachedEof = true;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    kLog.warn("IPC read failed: {}", std::strerror(errno));
    break;
  }

  if (command.size() >= kMaxCommandBytes && !reachedEof) {
    const std::string response = "error: IPC command too large\n";
    std::size_t sent = 0;
    while (sent < response.size()) {
      const auto n = ::send(connFd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        break;
      }
      sent += static_cast<std::size_t>(n);
    }
    return;
  }

  // Legacy external clients may still send one newline-terminated command and
  // keep the socket open while waiting for the response.
  if (!reachedEof) {
    if (const auto newline = command.find('\n'); newline != std::string::npos) {
      command.resize(newline);
    }
    if (!command.empty() && command.back() == '\r') {
      command.pop_back();
    }
  }

  if (command.empty()) {
    // Client closed the connection without sending anything (e.g. a liveness probe).
    return;
  }

  const std::string response = execute(command);
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
  std::ranges::iota(order, 0);
  std::ranges::sort(order, [this](std::size_t lhs, std::size_t rhs) {
    return m_handlers[lhs].first < m_handlers[rhs].first;
  });

  // Find the longest usage string for alignment
  std::size_t maxUsage = 0;
  for (const auto& [cmd, entry] : m_handlers) {
    if (entry.visibility == HandlerVisibility::Hidden) {
      continue;
    }
    const auto& u = entry.usage.empty() ? cmd : entry.usage;
    maxUsage = std::max(maxUsage, u.size());
  }

  std::string out = "Usage: noctalia msg <command> [args]\n\nCommands:\n";
  for (const auto index : order) {
    const auto& [cmd, entry] = m_handlers[index];
    if (entry.visibility == HandlerVisibility::Hidden) {
      continue;
    }
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
  const auto it = std::ranges::find_if(m_handlers, [&command](const auto& e) { return e.first == command; });
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
