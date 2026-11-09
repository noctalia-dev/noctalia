#include "compositors/zoolander/zoolander_workspace_backend.h"

#include "core/log.h"

#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("workspace_zoolander");
  constexpr auto kPollInterval = std::chrono::milliseconds(500);
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

  // Sends one command on a fresh connection and reads the reply until the
  // server closes it. Short timeouts keep a stalled compositor from blocking
  // the main loop.
  std::optional<std::string> request(const std::string& path, std::string_view command) {
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
      return std::nullopt;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return std::nullopt;
    }
    timeval timeout{.tv_sec = 0, .tv_usec = 200000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      return std::nullopt;
    }
    std::string line(command);
    line += '\n';
    if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(line.size())) {
      ::close(fd);
      return std::nullopt;
    }
    ::shutdown(fd, SHUT_WR);
    std::string reply;
    char buffer[8192];
    while (true) {
      const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n > 0) {
        reply.append(buffer, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0) {
        ::close(fd);
        return std::nullopt;
      }
      break;
    }
    ::close(fd);
    return reply;
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

void ZoolanderWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void ZoolanderWorkspaceBackend::setWindowChangeCallback(ChangeCallback callback) {
  m_windowChangeCallback = std::move(callback);
}

int ZoolanderWorkspaceBackend::pollTimeoutMs() const noexcept {
  const auto now = Clock::now();
  if (now >= m_nextPoll) {
    return 0;
  }
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(m_nextPoll - now).count()) + 1;
}

void ZoolanderWorkspaceBackend::dispatchPoll(short /*revents*/) {
  const auto now = Clock::now();
  if (now < m_nextPoll) {
    return;
  }
  m_nextPoll = now + kPollInterval;
  if (!refresh()) {
    return;
  }
  if (m_changeCallback) {
    m_changeCallback();
  }
  if (m_windowChangeCallback) {
    m_windowChangeCallback();
  }
}

bool ZoolanderWorkspaceBackend::refresh() {
  const auto reply = request(m_socketPath, "clients-json");
  const auto payload = reply ? jsonPayload(*reply) : std::nullopt;
  if (!payload) {
    if (!m_loggedFailure) {
      kLog.warn("no clients-json reply from {}", m_socketPath);
      m_loggedFailure = true;
    }
    return false;
  }
  m_loggedFailure = false;

  std::vector<Client> clients;
  try {
    const auto parsed = nlohmann::json::parse(*payload);
    if (!parsed.is_array()) {
      kLog.warn("clients-json is not an array");
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
    kLog.warn("clients-json parse failed: {}", error.what());
    return false;
  }

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
  m_clients.clear();
  m_occupied.clear();
  m_keysByOutput.clear();
  m_changeCallback = {};
  m_windowChangeCallback = {};
}
