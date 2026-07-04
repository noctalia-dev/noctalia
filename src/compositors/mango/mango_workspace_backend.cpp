#include "compositors/mango/mango_workspace_backend.h"

#include "compositors/mango/mango_runtime.h"
#include "core/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("workspace_mango");

  [[nodiscard]] std::string jsonString(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
  }

  [[nodiscard]] std::int32_t jsonInt(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end()) {
      return 0;
    }
    if (it->is_number_integer()) {
      return it->get<std::int32_t>();
    }
    if (it->is_number_unsigned()) {
      return static_cast<std::int32_t>(it->get<std::uint32_t>());
    }
    return 0;
  }

  [[nodiscard]] bool jsonBool(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    return it != json.end() && it->is_boolean() && it->get<bool>();
  }

  [[nodiscard]] std::string jsonIdString(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
      return {};
    }
    if (it->is_string()) {
      return it->get<std::string>();
    }
    if (it->is_number_unsigned()) {
      return std::to_string(it->get<std::uint64_t>());
    }
    if (it->is_number_integer()) {
      return std::to_string(it->get<std::int64_t>());
    }
    return {};
  }

  [[nodiscard]] std::vector<std::uint32_t> jsonTagArray(const nlohmann::json& json, const char* key) {
    std::vector<std::uint32_t> result;
    const auto it = json.find(key);
    if (it == json.end() || !it->is_array()) {
      return result;
    }
    result.reserve(it->size());
    for (const auto& item : *it) {
      if (item.is_number_unsigned()) {
        result.push_back(item.get<std::uint32_t>());
      } else if (item.is_number_integer()) {
        const auto value = item.get<std::int32_t>();
        if (value > 0) {
          result.push_back(static_cast<std::uint32_t>(value));
        }
      }
    }
    return result;
  }

  [[nodiscard]] bool sendAll(int fd, std::string_view payload) {
    std::size_t offset = 0;
    while (offset < payload.size()) {
      const ssize_t written = ::send(fd, payload.data() + offset, payload.size() - offset, MSG_NOSIGNAL);
      if (written <= 0) {
        if (written < 0 && errno == EINTR) {
          continue;
        }
        return false;
      }
      offset += static_cast<std::size_t>(written);
    }
    return true;
  }

} // namespace

MangoWorkspaceBackend::MangoWorkspaceBackend(compositors::mango::MangoRuntime& runtime) : m_runtime(runtime) {}

bool MangoWorkspaceBackend::isAvailable() const noexcept { return m_watchFd >= 0; }

void MangoWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void MangoWorkspaceBackend::setOutputNameResolver(WorkspaceOutputNameResolver::Resolver resolver) {
  m_outputNameResolver = std::move(resolver);
}

bool MangoWorkspaceBackend::connectSocket() {
  if (m_watchFd >= 0) {
    return true;
  }
  if (!openWatchSocket()) {
    return false;
  }
  refreshClients();
  syncFocusedClientTags();
  return true;
}

void MangoWorkspaceBackend::activate(const std::string& id) {
  if (const auto* active = activeOutputState(); active != nullptr && !active->name.empty()) {
    (void)m_runtime.dispatch("viewcrossmon," + id + "," + active->name);
    return;
  }
  (void)m_runtime.dispatch("view," + id);
}

void MangoWorkspaceBackend::activateForOutput(wl_output* output, const std::string& id) {
  const std::string name = outputName(output);
  if (!name.empty()) {
    (void)m_runtime.dispatch("viewcrossmon," + id + "," + name);
    return;
  }
  activate(id);
}

void MangoWorkspaceBackend::activateForOutput(wl_output* output, const Workspace& workspace) {
  const auto index = parseTagIndex(workspace);
  if (!index.has_value()) {
    return;
  }
  activateForOutput(output, std::to_string(*index + 1));
}

std::vector<Workspace> MangoWorkspaceBackend::all() const {
  const auto* state = activeOutputState();
  return state != nullptr ? forOutput(nullptr) : std::vector<Workspace>{};
}

std::vector<Workspace> MangoWorkspaceBackend::forOutput(wl_output* output) const {
  const auto* state = output != nullptr ? outputStateFor(output) : activeOutputState();
  if (state == nullptr) {
    return {};
  }

  const auto shellActive = shellActiveTagIndex(state->tags);
  std::vector<Workspace> result;
  result.reserve(state->tags.size());
  for (std::size_t i = 0; i < state->tags.size(); ++i) {
    result.push_back(makeWorkspace(state->tags[i], shellActive.has_value() && i == *shellActive));
  }
  return result;
}

std::unordered_map<std::string, std::vector<std::string>>
MangoWorkspaceBackend::appIdsByWorkspace(wl_output* output) const {
  std::unordered_map<std::string, std::vector<std::string>> result;
  const auto* state = output != nullptr ? outputStateFor(output) : nullptr;
  const std::string outputFilter = state != nullptr ? state->name : std::string{};
  for (const auto& client : m_clients) {
    if (!outputFilter.empty() && client.monitorName != outputFilter) {
      continue;
    }
    if (client.appId.empty()) {
      continue;
    }
    for (const std::uint32_t tag : client.tags) {
      if (tag == 0) {
        continue;
      }
      auto& apps = result[std::to_string(tag)];
      if (!std::ranges::contains(apps, client.appId)) {
        apps.push_back(client.appId);
      }
    }
  }
  return result;
}

std::vector<WorkspaceWindow> MangoWorkspaceBackend::workspaceWindows(wl_output* output) const {
  std::vector<WorkspaceWindow> result;
  const auto* state = output != nullptr ? outputStateFor(output) : nullptr;
  const std::string outputFilter = state != nullptr ? state->name : std::string{};
  for (const auto& client : m_clients) {
    if (!outputFilter.empty() && client.monitorName != outputFilter) {
      continue;
    }
    for (const std::uint32_t tag : client.tags) {
      if (tag == 0) {
        continue;
      }
      result.push_back(
          WorkspaceWindow{
              .windowId = client.id,
              .workspaceKey = std::to_string(tag),
              .appId = client.appId,
              .title = client.title,
              .x = client.x,
              .y = client.y,
              .outputName = {},
          }
      );
    }
  }
  return result;
}

void MangoWorkspaceBackend::focusWindow(const std::string& windowId) {
  if (!windowId.empty()) {
    (void)m_runtime.dispatch("focusid client," + windowId);
  }
}

void MangoWorkspaceBackend::cleanup() {
  closeWatchSocket();
  m_knownOutputs.clear();
  m_outputsByName.clear();
  m_clients.clear();
}

void MangoWorkspaceBackend::onOutputAdded(wl_output* output) {
  if (output == nullptr || std::ranges::contains(m_knownOutputs, output)) {
    return;
  }
  m_knownOutputs.push_back(output);
}

void MangoWorkspaceBackend::onOutputRemoved(wl_output* output) { std::erase(m_knownOutputs, output); }

int MangoWorkspaceBackend::pollFd() const noexcept { return m_watchFd; }

int MangoWorkspaceBackend::pollTimeoutMs() const noexcept { return m_watchFd >= 0 ? -1 : 2000; }

void MangoWorkspaceBackend::dispatchPoll(short revents) {
  if (m_watchFd < 0) {
    (void)connectSocket();
    return;
  }
  if ((revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
    closeWatchSocket();
    notifyChanged();
    return;
  }
  if ((revents & POLLIN) != 0) {
    readWatchSocket();
  }
}

wl_output* MangoWorkspaceBackend::ipcSelectedOutput() const {
  for (const auto& [name, state] : m_outputsByName) {
    if (!state.active) {
      continue;
    }
    for (wl_output* output : m_knownOutputs) {
      if (outputName(output) == name) {
        return output;
      }
    }
  }
  return nullptr;
}

std::optional<std::pair<std::string, std::string>>
MangoWorkspaceBackend::ipcFocusedClientForOutput(wl_output* output) const {
  const auto* state = outputStateFor(output);
  if (state == nullptr) {
    state = activeOutputState();
  }
  if (state == nullptr) {
    return std::nullopt;
  }
  return std::pair<std::string, std::string>{state->activeClientTitle, state->activeClientAppId};
}

bool MangoWorkspaceBackend::openWatchSocket() {
  const auto& socketPath = m_runtime.socketPath();
  if (socketPath.empty()) {
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socketPath.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return false;
  }
  std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);

  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return false;
  }

  if (!sendAll(fd, "watch all-monitors\n")) {
    ::close(fd);
    return false;
  }

  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  m_watchFd = fd;
  kLog.info("connected Mango IPC workspace socket={}", socketPath);
  return true;
}

void MangoWorkspaceBackend::closeWatchSocket() {
  if (m_watchFd >= 0) {
    ::close(m_watchFd);
    m_watchFd = -1;
  }
  m_readBuffer.clear();
}

void MangoWorkspaceBackend::readWatchSocket() {
  char buffer[8192];
  bool changed = false;
  while (true) {
    const ssize_t count = ::recv(m_watchFd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count > 0) {
      m_readBuffer.append(buffer, static_cast<std::size_t>(count));
      std::size_t newline = std::string::npos;
      while ((newline = m_readBuffer.find('\n')) != std::string::npos) {
        std::string line = m_readBuffer.substr(0, newline);
        m_readBuffer.erase(0, newline + 1);
        changed = handleMessage(line) || changed;
      }
      continue;
    }
    if (count == 0) {
      closeWatchSocket();
      changed = true;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      closeWatchSocket();
      changed = true;
    }
    break;
  }
  if (changed) {
    notifyChanged();
  }
}

bool MangoWorkspaceBackend::handleMessage(std::string_view line) {
  if (line.empty()) {
    return false;
  }

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(line);
  } catch (const nlohmann::json::exception&) {
    return false;
  }

  const auto monitorsIt = parsed.find("monitors");
  if (monitorsIt == parsed.end() || !monitorsIt->is_array()) {
    return false;
  }

  std::unordered_map<std::string, OutputState> nextOutputs;
  for (const auto& monitorJson : *monitorsIt) {
    auto monitor = parseMonitor(monitorJson);
    if (monitor.has_value() && !monitor->name.empty()) {
      nextOutputs.emplace(monitor->name, std::move(*monitor));
    }
  }

  m_outputsByName = std::move(nextOutputs);
  refreshClients();
  syncFocusedClientTags();
  return true;
}

void MangoWorkspaceBackend::refreshClients() {
  const auto response = m_runtime.request("get all-clients");
  if (!response.has_value() || !response->is_object()) {
    return;
  }

  const auto clientsIt = response->find("clients");
  if (clientsIt == response->end() || !clientsIt->is_array()) {
    return;
  }

  std::vector<ClientState> nextClients;
  nextClients.reserve(clientsIt->size());
  for (const auto& clientJson : *clientsIt) {
    auto client = parseClient(clientJson);
    if (client.has_value()) {
      nextClients.push_back(std::move(*client));
    }
  }
  m_clients = std::move(nextClients);
}

void MangoWorkspaceBackend::syncFocusedClientTags() {
  for (auto& [monitorName, state] : m_outputsByName) {
    for (auto& tag : state.tags) {
      tag.hasFocusedClient = false;
    }

    const ClientState* focusedClient = nullptr;
    if (!state.activeClientId.empty()) {
      for (const auto& client : m_clients) {
        if (client.id == state.activeClientId && client.monitorName == monitorName) {
          focusedClient = &client;
          break;
        }
      }
    }
    if (focusedClient == nullptr) {
      for (const auto& client : m_clients) {
        if (client.focused && client.monitorName == monitorName) {
          focusedClient = &client;
          break;
        }
      }
    }
    if (focusedClient == nullptr) {
      continue;
    }

    for (const std::uint32_t tagIndex : focusedClient->tags) {
      if (tagIndex == 0) {
        continue;
      }
      for (auto& tag : state.tags) {
        if (tag.index == tagIndex) {
          tag.hasFocusedClient = true;
        }
      }
    }
  }
}

void MangoWorkspaceBackend::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

std::string MangoWorkspaceBackend::outputName(wl_output* output) const {
  return m_outputNameResolver && output != nullptr ? m_outputNameResolver(output) : std::string{};
}

MangoWorkspaceBackend::OutputState* MangoWorkspaceBackend::activeOutputState() {
  for (auto& [_, state] : m_outputsByName) {
    if (state.active) {
      return &state;
    }
  }
  return !m_outputsByName.empty() ? &m_outputsByName.begin()->second : nullptr;
}

const MangoWorkspaceBackend::OutputState* MangoWorkspaceBackend::activeOutputState() const {
  for (const auto& [_, state] : m_outputsByName) {
    if (state.active) {
      return &state;
    }
  }
  return !m_outputsByName.empty() ? &m_outputsByName.begin()->second : nullptr;
}

const MangoWorkspaceBackend::OutputState* MangoWorkspaceBackend::outputStateFor(wl_output* output) const {
  const std::string name = outputName(output);
  if (name.empty()) {
    return nullptr;
  }
  const auto it = m_outputsByName.find(name);
  return it != m_outputsByName.end() ? &it->second : nullptr;
}

std::optional<std::size_t> MangoWorkspaceBackend::parseTagIndex(const Workspace& workspace) {
  if (!workspace.coordinates.empty()) {
    return static_cast<std::size_t>(workspace.coordinates[0]);
  }
  return parseTagIndex(workspace.id.empty() ? workspace.name : workspace.id);
}

std::optional<std::size_t> MangoWorkspaceBackend::parseTagIndex(const std::string& id) {
  if (id.empty()) {
    return std::nullopt;
  }

  std::size_t value = 0;
  const char* start = id.data();
  const char* end = id.data() + id.size();
  const auto [ptr, ec] = std::from_chars(start, end, value);
  if (ec != std::errc{} || ptr != end || value == 0) {
    return std::nullopt;
  }
  return value - 1;
}

std::optional<std::size_t> MangoWorkspaceBackend::shellActiveTagIndex(const std::vector<TagInfo>& tags) const {
  std::vector<std::size_t> activeTags;
  activeTags.reserve(tags.size());
  for (std::size_t i = 0; i < tags.size(); ++i) {
    if (tags[i].active) {
      activeTags.push_back(i);
    }
  }
  if (activeTags.empty()) {
    return std::nullopt;
  }
  if (activeTags.size() == 1) {
    return activeTags.front();
  }
  for (const std::size_t i : activeTags) {
    if (tags[i].hasFocusedClient) {
      return i;
    }
  }
  return activeTags.front();
}

Workspace MangoWorkspaceBackend::makeWorkspace(const TagInfo& tag, bool shellActive) {
  return Workspace{
      .id = std::to_string(tag.index),
      .name = std::to_string(tag.index),
      .coordinates = {tag.index > 0 ? tag.index - 1 : 0},
      .index = tag.index,
      .active = shellActive,
      .urgent = tag.urgent,
      .occupied = tag.occupied,
  };
}

std::optional<MangoWorkspaceBackend::OutputState> MangoWorkspaceBackend::parseMonitor(const nlohmann::json& json) {
  if (!json.is_object()) {
    return std::nullopt;
  }

  OutputState state{};
  state.name = jsonString(json, "name");
  state.active = jsonBool(json, "active");
  state.x = jsonInt(json, "x");
  state.y = jsonInt(json, "y");
  state.width = jsonInt(json, "width");
  state.height = jsonInt(json, "height");

  const auto activeClientIt = json.find("active_client");
  if (activeClientIt != json.end() && activeClientIt->is_object()) {
    state.activeClientId = jsonIdString(*activeClientIt, "id");
    state.activeClientTitle = StringUtils::windowTitleSingleLine(jsonString(*activeClientIt, "title"));
    state.activeClientAppId = jsonString(*activeClientIt, "appid");
  }

  const auto tagsIt = json.find("tags");
  if (tagsIt != json.end() && tagsIt->is_array()) {
    state.tags.reserve(tagsIt->size());
    for (const auto& tagJson : *tagsIt) {
      if (!tagJson.is_object()) {
        continue;
      }
      TagInfo tag{};
      tag.index = static_cast<std::uint32_t>(jsonInt(tagJson, "index"));
      tag.active = jsonBool(tagJson, "is_active");
      tag.urgent = jsonBool(tagJson, "is_urgent");
      tag.occupied = jsonInt(tagJson, "client_count") > 0;
      state.tags.push_back(tag);
    }
  }

  const auto activeTags = jsonTagArray(json, "active_tags");
  if (!activeTags.empty() && !(activeTags.size() == 1 && activeTags.front() == 0)) {
    for (auto& tag : state.tags) {
      tag.active = std::ranges::contains(activeTags, tag.index);
    }
  }

  for (auto& tag : state.tags) {
    tag.hasFocusedClient = false;
  }

  return state;
}

std::optional<MangoWorkspaceBackend::ClientState> MangoWorkspaceBackend::parseClient(const nlohmann::json& json) {
  if (!json.is_object()) {
    return std::nullopt;
  }

  ClientState client{};
  const auto idIt = json.find("id");
  if (idIt != json.end()) {
    if (idIt->is_number_unsigned()) {
      client.id = std::to_string(idIt->get<std::uint64_t>());
    } else if (idIt->is_number_integer()) {
      client.id = std::to_string(idIt->get<std::int64_t>());
    }
  }
  client.title = StringUtils::windowTitleSingleLine(jsonString(json, "title"));
  client.appId = jsonString(json, "appid");
  client.monitorName = jsonString(json, "monitor");
  client.tags = jsonTagArray(json, "tags");
  client.focused = jsonBool(json, "is_focused");
  client.x = jsonInt(json, "x");
  client.y = jsonInt(json, "y");
  return client;
}
