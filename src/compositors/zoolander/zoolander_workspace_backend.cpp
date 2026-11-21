#include "compositors/zoolander/zoolander_workspace_backend.h"

#include "core/log.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("workspace_zoolander");
  constexpr auto kPollInterval = std::chrono::milliseconds(500);
  constexpr auto kRequestTimeout = std::chrono::milliseconds(1000);
  constexpr std::size_t kMaxReplyBytes = 4 * 1024 * 1024;
  constexpr std::string_view kCommand = "clients-json\n";
  constexpr std::string_view kGreeting = "ZLCTL 1";
  constexpr std::string_view kJsonReply = "OK json";

  std::string socketPath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* display = std::getenv("WAYLAND_DISPLAY");
    if (runtime == nullptr || runtime[0] == '\0' || display == nullptr || display[0] == '\0') {
      return {};
    }
    std::string name(display);
    if (const auto slash = name.rfind('/'); slash != std::string::npos) {
      name = name.substr(slash + 1);
    }
    return std::string(runtime) + "/zoolander-" + name + ".sock";
  }

  int msUntil(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return 0;
    }
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()) + 1;
  }

  // Splits "ZLCTL 1\nOK json\n<json>" and returns the JSON line.
  std::optional<std::string_view> jsonPayload(std::string_view reply) {
    const auto greetingEnd = reply.find('\n');
    if (greetingEnd == std::string_view::npos || reply.substr(0, greetingEnd) != kGreeting) {
      return std::nullopt;
    }
    const auto statusStart = greetingEnd + 1;
    const auto statusEnd = reply.find('\n', statusStart);
    if (statusEnd == std::string_view::npos || reply.substr(statusStart, statusEnd - statusStart) != kJsonReply) {
      return std::nullopt;
    }
    return reply.substr(statusEnd + 1);
  }

} // namespace

ZoolanderWorkspaceBackend::ZoolanderWorkspaceBackend() : m_socketPath(socketPath()) {}

ZoolanderWorkspaceBackend::~ZoolanderWorkspaceBackend() { closeRequest(); }

void ZoolanderWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void ZoolanderWorkspaceBackend::setWindowChangeCallback(ChangeCallback callback) {
  m_windowChangeCallback = std::move(callback);
}

int ZoolanderWorkspaceBackend::pollFd() const noexcept { return m_fd; }

int ZoolanderWorkspaceBackend::pollTimeoutMs() const noexcept {
  return msUntil(m_fd >= 0 ? m_requestDeadline : m_nextPoll);
}

void ZoolanderWorkspaceBackend::dispatchPoll(short revents) {
  const auto now = Clock::now();
  if (m_fd >= 0) {
    if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      readReply();
    } else if (now >= m_requestDeadline) {
      closeRequest();
      reportFailure("clients-json request timed out");
    }
    return;
  }
  if (now >= m_nextPoll) {
    m_nextPoll = now + kPollInterval;
    startRequest();
  }
}

void ZoolanderWorkspaceBackend::startRequest() {
  sockaddr_un addr{};
  if (m_socketPath.empty() || m_socketPath.size() >= sizeof(addr.sun_path)) {
    reportFailure("no usable control socket path");
    return;
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    reportFailure("socket() failed");
    return;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);
  // The command fits in an empty socket buffer, so the send completes or fails at once.
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
      || ::send(fd, kCommand.data(), kCommand.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(kCommand.size())) {
    ::close(fd);
    reportFailure("cannot reach control socket");
    return;
  }
  ::shutdown(fd, SHUT_WR);
  m_fd = fd;
  m_reply.clear();
  m_requestDeadline = Clock::now() + kRequestTimeout;
}

void ZoolanderWorkspaceBackend::readReply() {
  char buffer[8192];
  ssize_t n = 0;
  while ((n = ::recv(m_fd, buffer, sizeof(buffer), 0)) > 0 || (n < 0 && errno == EINTR)) {
    if (n <= 0) {
      continue;
    }
    m_reply.append(buffer, static_cast<std::size_t>(n));
    if (m_reply.size() > kMaxReplyBytes) {
      closeRequest();
      reportFailure("clients-json reply too large");
      return;
    }
  }
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return;
  }
  closeRequest();
  if (n < 0) {
    reportFailure("clients-json read failed");
    return;
  }
  // n == 0: the server closed the connection after its reply.
  if (!applyReply()) {
    return;
  }
  if (m_changeCallback) {
    m_changeCallback();
  }
  if (m_windowChangeCallback) {
    m_windowChangeCallback();
  }
}

void ZoolanderWorkspaceBackend::closeRequest() noexcept {
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
}

void ZoolanderWorkspaceBackend::reportFailure(std::string_view reason) {
  if (!m_loggedFailure) {
    kLog.warn("{} ({}); keeping the last known windows", reason, m_socketPath);
    m_loggedFailure = true;
  }
}

bool ZoolanderWorkspaceBackend::applyReply() {
  const auto payload = jsonPayload(m_reply);
  if (!payload) {
    reportFailure("unexpected clients-json reply");
    return false;
  }

  std::vector<Client> clients;
  try {
    const auto parsed = nlohmann::json::parse(*payload);
    if (!parsed.is_array()) {
      reportFailure("clients-json is not an array");
      return false;
    }
    const auto text = [](const nlohmann::json& object, const char* key) {
      const auto it = object.find(key);
      return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    const auto number = [](const nlohmann::json& object, const char* key) {
      const auto it = object.find(key);
      return it != object.end() && it->is_number_integer() ? it->get<std::int32_t>() : 0;
    };
    for (const auto& object : parsed) {
      // Special workspaces (scratchpads) are not part of the ext-workspace list.
      if (!object.is_object() || object.value("special", false)) {
        continue;
      }
      Client client{
          .id = text(object, "id"),
          .workspace = text(object, "workspace"),
          .appId = text(object, "app_id"),
          .title = text(object, "title"),
          .output = text(object, "output"),
          .x = number(object, "x"),
          .y = number(object, "y"),
          .minimized = object.value("minimized", false),
      };
      if (!client.workspace.empty()) {
        clients.push_back(std::move(client));
      }
    }
  } catch (const nlohmann::json::exception& error) {
    reportFailure(error.what());
    return false;
  }
  m_loggedFailure = false;

  if (clients == m_clients) {
    return false;
  }
  m_clients = std::move(clients);
  m_occupied.clear();
  for (const auto& client : m_clients) {
    m_occupied.insert(client.workspace);
  }
  return true;
}

std::string ZoolanderWorkspaceBackend::workspaceKey(const Workspace& workspace) {
  return workspace.name.empty() ? workspace.id : workspace.name;
}

void ZoolanderWorkspaceBackend::apply(std::vector<Workspace>& workspaces, const std::string& outputName) const {
  std::vector<std::string> keys;
  keys.reserve(workspaces.size());
  for (auto& workspace : workspaces) {
    const std::string key = workspaceKey(workspace);
    if (m_occupied.contains(key)) {
      workspace.occupied = true;
    }
    keys.push_back(key);
  }
  for (auto& [cachedOutput, cached] : m_keysByOutput) {
    if (cachedOutput == outputName) {
      cached = std::move(keys);
      return;
    }
  }
  m_keysByOutput.emplace_back(outputName, std::move(keys));
}

std::vector<std::string> ZoolanderWorkspaceBackend::workspaceKeys(const std::string& outputName) const {
  for (const auto& [cachedOutput, keys] : m_keysByOutput) {
    if (cachedOutput == outputName) {
      return keys;
    }
  }
  return {};
}

std::unordered_map<std::string, std::vector<std::string>>
ZoolanderWorkspaceBackend::appIdsByWorkspace(const std::string& outputName) const {
  std::unordered_map<std::string, std::vector<std::string>> result;
  for (const auto& client : m_clients) {
    if (!outputName.empty() && client.output != outputName) {
      continue;
    }
    if (!client.appId.empty()) {
      result[client.workspace].push_back(client.appId);
    }
  }
  return result;
}

std::vector<WorkspaceWindow> ZoolanderWorkspaceBackend::workspaceWindows(const std::string& outputName) const {
  std::vector<WorkspaceWindow> result;
  result.reserve(m_clients.size());
  for (const auto& client : m_clients) {
    if (!outputName.empty() && client.output != outputName) {
      continue;
    }
    result.push_back(
        WorkspaceWindow{
            .windowId = client.id,
            .workspaceKey = client.workspace,
            .appId = client.appId,
            .title = client.title,
            .x = client.x,
            .y = client.y,
            .outputName = client.output,
            .minimizeState = WindowMinimizeState{.minimized = client.minimized, .minimizable = true},
        }
    );
  }
  return result;
}

void ZoolanderWorkspaceBackend::cleanup() {
  closeRequest();
  m_clients.clear();
  m_occupied.clear();
  m_keysByOutput.clear();
  m_changeCallback = {};
  m_windowChangeCallback = {};
}
