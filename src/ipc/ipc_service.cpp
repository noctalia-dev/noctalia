#include "ipc/ipc_service.h"

#include "cli/help.h"
#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
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

IpcService::InvocationScope::InvocationScope(const IpcService& ipc, std::optional<IpcInvocationContext> context)
    : m_ipc(ipc), m_previous(std::move(ipc.m_invocationContext)) {
  m_ipc.m_invocationContext = std::move(context);
}

IpcService::InvocationScope::~InvocationScope() { m_ipc.m_invocationContext = std::move(m_previous); }

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

void IpcService::bind(const noctalia::cli::Command& command, Handler handler, HandlerOptions options) {
  const noctalia::cli::Command* spec = noctalia::cli::findMsgCommand(command.name);
  if (spec == nullptr) {
    kLog.error("cannot bind non-msg CLI command '{}'", command.name);
    return;
  }

  std::erase_if(m_handlers, [spec](const auto& entry) { return entry.first == spec->name; });
  m_handlers.emplace_back(
      spec->name,
      HandlerEntry{
          .fn = std::move(handler),
          .argsSpec = noctalia::cli::renderArgsSpec(*spec),
          .description = spec->summary,
          .actionEditorVisibility = options.actionEditorVisibility,
          .cycles = false,
      }
  );
}

void IpcService::bindCycle(const noctalia::cli::Command& command, Handler handler, HandlerOptions options) {
  bind(command, std::move(handler), options);
  const auto it =
      std::ranges::find_if(m_handlers, [&command](const auto& entry) { return entry.first == command.name; });
  if (it != m_handlers.end())
    it->second.cycles = true;
}

bool IpcService::handlerCycles(std::string_view command) const noexcept {
  const auto it = std::ranges::find_if(m_handlers, [command](const auto& e) { return e.first == command; });
  return it != m_handlers.end() && it->second.cycles;
}

std::vector<IpcService::HandlerInfo> IpcService::handlers() const {
  std::vector<HandlerInfo> infos;
  infos.reserve(m_handlers.size());
  for (const auto& [command, entry] : m_handlers) {
    infos.push_back(
        HandlerInfo{
            .command = command,
            .args = entry.argsSpec,
            .description = entry.description,
            .actionEditorVisibility = entry.actionEditorVisibility,
            .cycles = entry.cycles,
        }
    );
  }
  std::ranges::sort(infos, {}, &HandlerInfo::command);
  return infos;
}

bool IpcService::hasHandler(std::string_view command) const noexcept {
  return std::ranges::any_of(m_handlers, [command](const auto& entry) { return entry.first == command; });
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

  // A socket command has no in-process origin, even when the dispatch re-enters from a handler
  // that pumped the event loop.
  const InvocationScope socketScope(*this, std::nullopt);
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

std::string IpcService::HandlerInfo::signature() const {
  std::string out(command);
  if (!args.empty()) {
    out += ' ';
    out += args;
  }
  return out;
}

std::string IpcService::buildHelp() const {
  auto infos = handlers();

  std::vector<std::string> signatures;
  signatures.reserve(infos.size());
  std::size_t maxSignature = 0;
  for (const auto& info : infos) {
    signatures.push_back(info.signature());
    maxSignature = std::max(maxSignature, signatures.back().size());
  }

  std::string out = "Usage: noctalia msg <command> [args]\n\nCommands:\n";
  for (std::size_t i = 0; i < infos.size(); ++i) {
    out += "  ";
    out += signatures[i];
    if (!infos[i].description.empty()) {
      out += std::string(maxSignature - signatures[i].size() + 2, ' ');
      out += infos[i].description;
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
