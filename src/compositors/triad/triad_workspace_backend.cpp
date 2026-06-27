#include "compositors/triad/triad_workspace_backend.h"

#include "compositors/triad/triad_runtime.h"
#include "core/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <json.hpp>
#include <limits>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_set>

namespace {

  constexpr Logger kLog("triad_workspace");
  constexpr auto kReconnectInitial = std::chrono::seconds(2);
  constexpr auto kReconnectMax = std::chrono::seconds(30);
  constexpr std::size_t kReadBufferMaxBytes = 1024U * 1024U;
  constexpr std::string_view kEventStreamRequest =
      "{\"triad\":{\"version\":1,\"request\":\"event-stream\",\"events\":[\"state\",\"layout\",\"window\"]}}\n";

  [[nodiscard]] bool writeAll(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
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

  [[nodiscard]] std::optional<std::int32_t> jsonInt32(const nlohmann::json& json) {
    if (json.is_number_integer()) {
      const auto value = json.get<std::int64_t>();
      if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
      }
      return static_cast<std::int32_t>(value);
    }
    if (json.is_number_unsigned()) {
      const auto value = json.get<std::uint64_t>();
      if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
      }
      return static_cast<std::int32_t>(value);
    }
    return std::nullopt;
  }

  [[nodiscard]] const nlohmann::json* triadObject(const nlohmann::json& message) {
    if (!message.is_object()) {
      return nullptr;
    }
    const auto it = message.find("triad");
    return it != message.end() && it->is_object() ? &(*it) : nullptr;
  }

  [[nodiscard]] const nlohmann::json* objectField(const nlohmann::json& json, const char* key) {
    if (!json.is_object()) {
      return nullptr;
    }
    const auto it = json.find(key);
    return it != json.end() && it->is_object() ? &(*it) : nullptr;
  }

  [[nodiscard]] const nlohmann::json* arrayField(const nlohmann::json& json, const char* key) {
    if (!json.is_object()) {
      return nullptr;
    }
    const auto it = json.find(key);
    return it != json.end() && it->is_array() ? &(*it) : nullptr;
  }

} // namespace

TriadWorkspaceBackend::TriadWorkspaceBackend(compositors::triad::TriadRuntime& runtime) : m_runtime(runtime) {
  if (m_runtime.available()) {
    refreshSnapshot();
    (void)connectSocket();
  }
}

TriadWorkspaceBackend::~TriadWorkspaceBackend() { cleanup(); }

bool TriadWorkspaceBackend::isAvailable() const noexcept { return m_runtime.available(); }

bool TriadWorkspaceBackend::canTrackOverviewState() const noexcept { return m_runtime.available(); }

void TriadWorkspaceBackend::setChangeCallback(WorkspaceBackend::ChangeCallback callback) {
  m_changeCallback = std::move(callback);
}

void TriadWorkspaceBackend::setOverviewChangeCallback(compositors::WorkspaceMetadataBackend::ChangeCallback callback) {
  m_overviewChangeCallback = std::move(callback);
}

void TriadWorkspaceBackend::setOutputNameResolver(Resolver resolver) { m_outputNameResolver = std::move(resolver); }

bool TriadWorkspaceBackend::connectSocket() {
  const auto& socketPath = m_runtime.socketPath();
  if (m_socketFd >= 0) {
    return true;
  }
  if (socketPath.empty()) {
    return false;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_nextReconnectAt.time_since_epoch().count() != 0 && now < m_nextReconnectAt) {
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    scheduleReconnect();
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socketPath.size() >= sizeof(addr.sun_path)) {
    kLog.warn("triad socket path too long");
    ::close(fd);
    scheduleReconnect();
    return false;
  }
  std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);

  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    scheduleReconnect();
    return false;
  }

  if (!writeAll(fd, kEventStreamRequest)) {
    ::close(fd);
    scheduleReconnect();
    return false;
  }

  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  m_socketFd = fd;
  m_nextReconnectAt = {};
  m_reconnectBackoff = kReconnectInitial;
  m_readBuffer.clear();
  kLog.debug("connected to triad event stream");
  return true;
}

void TriadWorkspaceBackend::activate(const std::string& id) {
  if (const auto index = parseWorkspaceIndex(id); index.has_value()) {
    (void)m_runtime.requestAction("focus-workspace", nlohmann::json{{"workspace_idx", *index}});
  }
}

void TriadWorkspaceBackend::activateForOutput(wl_output* /*output*/, const std::string& id) { activate(id); }

void TriadWorkspaceBackend::activateForOutput(wl_output* /*output*/, const Workspace& workspace) {
  if (workspace.index > 0) {
    (void)m_runtime.requestAction("focus-workspace", nlohmann::json{{"workspace_idx", workspace.index}});
    return;
  }
  activate(workspace.id);
}

std::vector<Workspace> TriadWorkspaceBackend::all() const {
  std::vector<Workspace> result;
  const auto workspaces = sortedWorkspaces();
  result.reserve(workspaces.size());
  for (const auto* workspace : workspaces) {
    result.push_back(
        Workspace{
            .id = workspaceKey(*workspace),
            .name = workspace->name.empty() ? workspaceKey(*workspace) : workspace->name,
            .coordinates = {workspace->index},
            .index = workspace->index,
            .active = workspace->active,
            .urgent = workspace->urgent,
            .occupied = workspace->occupied,
        }
    );
  }
  return result;
}

std::vector<Workspace> TriadWorkspaceBackend::forOutput(wl_output* output) const {
  std::vector<Workspace> result;
  const auto workspaces = sortedWorkspaces(outputNameFor(output));
  result.reserve(workspaces.size());
  for (const auto* workspace : workspaces) {
    result.push_back(
        Workspace{
            .id = workspaceKey(*workspace),
            .name = workspace->name.empty() ? workspaceKey(*workspace) : workspace->name,
            .coordinates = {workspace->index},
            .index = workspace->index,
            .active = workspace->active,
            .urgent = workspace->urgent,
            .occupied = workspace->occupied,
        }
    );
  }
  return result;
}

std::unordered_map<std::string, std::vector<std::string>>
TriadWorkspaceBackend::appIdsByWorkspace(wl_output* output) const {
  return appIdsByWorkspace(outputNameFor(output));
}

std::unordered_map<std::string, std::vector<std::string>>
TriadWorkspaceBackend::appIdsByWorkspace(const std::string& outputName) const {
  std::unordered_map<std::uint32_t, const WorkspaceState*> workspacesByIndex;
  for (const auto* workspace : sortedWorkspaces(outputName)) {
    workspacesByIndex.emplace(workspace->index, workspace);
  }

  std::unordered_map<std::string, std::vector<std::string>> result;
  std::unordered_map<std::string, std::unordered_set<std::string>> seen;
  for (const auto& [windowId, window] : m_windows) {
    (void)windowId;
    if (window.workspaceIndex == 0 || window.appId.empty()) {
      continue;
    }
    const auto workspaceIt = workspacesByIndex.find(window.workspaceIndex);
    if (workspaceIt == workspacesByIndex.end()) {
      continue;
    }
    const auto key = workspaceKey(*workspaceIt->second);
    if (seen[key].insert(window.appId).second) {
      result[key].push_back(window.appId);
    }
  }
  return result;
}

std::vector<WorkspaceWindow> TriadWorkspaceBackend::workspaceWindows(wl_output* output) const {
  return workspaceWindows(outputNameFor(output));
}

std::vector<WorkspaceWindow> TriadWorkspaceBackend::workspaceWindows(const std::string& outputName) const {
  std::unordered_map<std::uint32_t, const WorkspaceState*> workspacesByIndex;
  for (const auto* workspace : sortedWorkspaces(outputName)) {
    workspacesByIndex.emplace(workspace->index, workspace);
  }

  std::vector<WorkspaceWindow> result;
  result.reserve(m_windows.size());
  for (const auto& [windowId, window] : m_windows) {
    (void)windowId;
    const auto workspaceIt = workspacesByIndex.find(window.workspaceIndex);
    if (workspaceIt == workspacesByIndex.end()) {
      continue;
    }
    result.push_back(
        WorkspaceWindow{
            .windowId = std::to_string(window.id),
            .workspaceKey = workspaceKey(*workspaceIt->second),
            .appId = window.appId,
            .title = window.title,
            .x = window.x,
            .y = window.y,
            .outputName = {},
        }
    );
  }
  return result;
}

void TriadWorkspaceBackend::focusWindow(const std::string& windowId) {
  const auto parsed = parseUnsignedId(windowId);
  if (!parsed.has_value()) {
    return;
  }
  (void)m_runtime.requestAction("focus-window", nlohmann::json{{"id", *parsed}});
}

void TriadWorkspaceBackend::cleanup() {
  closeSocket(false);
  const bool overviewWasOpen = m_overviewKnown && m_overviewOpen;
  m_outputNames.clear();
  m_workspaces.clear();
  m_windows.clear();
  m_overviewKnown = false;
  m_overviewOpen = false;
  m_readBuffer.clear();
  m_reconnectBackoff = kReconnectInitial;
  if (overviewWasOpen) {
    notifyOverviewChanged();
  }
}

int TriadWorkspaceBackend::pollTimeoutMs() const noexcept {
  if (m_socketFd >= 0 || !m_runtime.available()) {
    return -1;
  }
  if (m_nextReconnectAt.time_since_epoch().count() == 0) {
    return 0;
  }

  const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(m_nextReconnectAt - std::chrono::steady_clock::now()).count();
  return static_cast<int>(std::max<std::int64_t>(0, remaining));
}

void TriadWorkspaceBackend::dispatchPoll(short revents) {
  if (!m_runtime.available()) {
    return;
  }

  if (m_socketFd < 0) {
    (void)connectSocket();
    return;
  }

  if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    closeSocket(true);
    return;
  }

  if ((revents & POLLIN) != 0) {
    readSocket();
  }
}

void TriadWorkspaceBackend::apply(std::vector<Workspace>& workspaces, const std::string& outputName) const {
  if (workspaces.empty() || m_workspaces.empty()) {
    return;
  }

  const auto candidates = sortedWorkspaces(outputName);
  for (auto& workspace : workspaces) {
    const auto parsed = parseWorkspaceIndex(workspace.id);
    const auto found = std::ranges::find_if(candidates, [&](const WorkspaceState* candidate) {
      if (parsed.has_value() && candidate->index == *parsed) {
        return true;
      }
      return !workspace.name.empty() && workspace.name == candidate->name;
    });
    if (found == candidates.end()) {
      workspace.index = 0;
      workspace.occupied = false;
      continue;
    }
    workspace.index = (*found)->index;
    workspace.occupied = (*found)->occupied;
    workspace.urgent = (*found)->urgent;
  }
}

std::vector<std::string> TriadWorkspaceBackend::workspaceKeys(const std::string& outputName) const {
  const auto candidates = sortedWorkspaces(outputName);
  std::vector<std::string> result;
  result.reserve(candidates.size());
  for (const auto* workspace : candidates) {
    result.push_back(workspaceKey(*workspace));
  }
  return result;
}

std::optional<std::string> TriadWorkspaceBackend::focusedWindowId() const {
  for (const auto& [index, workspace] : m_workspaces) {
    (void)index;
    if (workspace.globalActive && workspace.focusedWindowId.has_value()) {
      return std::to_string(*workspace.focusedWindowId);
    }
  }
  return std::nullopt;
}

void TriadWorkspaceBackend::closeSocket(bool scheduleReconnectFlag) {
  if (m_socketFd >= 0) {
    ::close(m_socketFd);
    m_socketFd = -1;
  }

  if (scheduleReconnectFlag) {
    scheduleReconnect();
  } else {
    m_nextReconnectAt = {};
  }
}

void TriadWorkspaceBackend::scheduleReconnect() {
  const auto now = std::chrono::steady_clock::now();
  m_nextReconnectAt = now + m_reconnectBackoff;
  const auto doubled = m_reconnectBackoff * 2;
  m_reconnectBackoff = std::min(doubled, kReconnectMax);
}

void TriadWorkspaceBackend::readSocket() {
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t readBytes = ::read(m_socketFd, buffer.data(), buffer.size());
    if (readBytes > 0) {
      m_readBuffer.insert(m_readBuffer.end(), buffer.begin(), buffer.begin() + readBytes);
      if (m_readBuffer.size() > kReadBufferMaxBytes) {
        kLog.warn("triad event stream read buffer exceeded {} bytes; reconnecting", kReadBufferMaxBytes);
        closeSocket(true);
        m_readBuffer.clear();
        return;
      }
      continue;
    }

    if (readBytes == 0) {
      closeSocket(true);
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }

    closeSocket(true);
    return;
  }

  parseMessages();
}

void TriadWorkspaceBackend::parseMessages() {
  auto lineStart = m_readBuffer.begin();
  for (auto it = m_readBuffer.begin(); it != m_readBuffer.end(); ++it) {
    if (*it != '\n') {
      continue;
    }

    std::string line(lineStart, it);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (!line.empty() && !handleMessage(line)) {
      m_readBuffer.clear();
      return;
    }

    lineStart = std::next(it);
  }

  if (lineStart != m_readBuffer.begin()) {
    m_readBuffer.erase(m_readBuffer.begin(), lineStart);
  }
}

bool TriadWorkspaceBackend::handleMessage(std::string_view line) {
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(line);
  } catch (const nlohmann::json::exception& e) {
    kLog.warn("failed to parse triad event stream message: {}", e.what());
    return true;
  }

  const auto* triad = triadObject(json);
  if (triad == nullptr) {
    return true;
  }

  if (const auto okIt = triad->find("ok"); okIt != triad->end() && okIt->is_boolean() && !okIt->get<bool>()) {
    kLog.warn("triad event stream returned an error, reconnecting");
    closeSocket(true);
    return false;
  }

  bool changed = false;
  bool overviewChanged = false;
  if (const auto stateIt = triad->find("state"); stateIt != triad->end() && stateIt->is_object()) {
    const bool beforeKnown = m_overviewKnown;
    const bool beforeOpen = m_overviewOpen;
    changed = applyTriadState(*stateIt);
    overviewChanged = beforeKnown != m_overviewKnown || beforeOpen != m_overviewOpen;
  } else if (const auto windowIt = triad->find("window"); windowIt != triad->end() && windowIt->is_object()) {
    changed = applyWindow(*windowIt);
  }

  if (changed) {
    notifyChanged();
  }
  if (overviewChanged) {
    notifyOverviewChanged();
  }
  return true;
}

bool TriadWorkspaceBackend::applyTriadState(const nlohmann::json& state) {
  bool changed = false;

  if (const auto* overview = objectField(state, "overview"); overview != nullptr) {
    const auto openIt = overview->find("is_open");
    if (openIt != overview->end() && openIt->is_boolean()) {
      const bool open = openIt->get<bool>();
      changed = changed || !m_overviewKnown || m_overviewOpen != open;
      m_overviewKnown = true;
      m_overviewOpen = open;
    }
  }

  if (const auto* outputs = arrayField(state, "outputs"); outputs != nullptr && applyOutputs(*outputs)) {
    changed = true;
  }
  if (applyLayoutState(state)) {
    changed = true;
  }
  if (const auto* layout = objectField(state, "layout"); layout != nullptr && applyLayoutState(*layout)) {
    changed = true;
  }
  if (const auto* windows = arrayField(state, "windows"); windows != nullptr && applyWindows(*windows)) {
    changed = true;
  }
  return changed;
}

bool TriadWorkspaceBackend::applyOutputs(const nlohmann::json& outputs) {
  if (!outputs.is_array()) {
    return false;
  }

  std::unordered_set<std::string> next;
  for (const auto& output : outputs) {
    const auto name = jsonString(output, "name");
    if (!name.empty()) {
      next.insert(name);
    }
  }

  if (next == m_outputNames) {
    return false;
  }

  m_outputNames = std::move(next);
  return true;
}

bool TriadWorkspaceBackend::applyLayoutState(const nlohmann::json& state) {
  const auto* workspaces = arrayField(state, "workspaces");
  if (workspaces == nullptr) {
    return false;
  }

  std::unordered_map<std::uint32_t, WorkspaceState> next;
  for (const auto& item : *workspaces) {
    if (const auto workspace = parseWorkspace(item); workspace.has_value() && workspace->index > 0) {
      next.emplace(workspace->index, *workspace);
    }
  }

  if (next.size() == m_workspaces.size()) {
    bool same = true;
    for (const auto& [index, workspace] : next) {
      const auto oldIt = m_workspaces.find(index);
      if (oldIt == m_workspaces.end()
          || oldIt->second.name != workspace.name
          || oldIt->second.output != workspace.output
          || oldIt->second.active != workspace.active
          || oldIt->second.globalActive != workspace.globalActive
          || oldIt->second.urgent != workspace.urgent
          || oldIt->second.occupied != workspace.occupied
          || oldIt->second.focusedWindowId != workspace.focusedWindowId) {
        same = false;
        break;
      }
    }
    if (same) {
      return false;
    }
  }

  m_workspaces = std::move(next);
  return true;
}

bool TriadWorkspaceBackend::applyWindows(const nlohmann::json& windows) {
  if (!windows.is_array()) {
    return false;
  }

  std::unordered_map<std::uint64_t, WindowState> next;
  for (const auto& item : windows) {
    if (const auto window = parseWindow(item); window.has_value()) {
      next.emplace(window->id, *window);
    }
  }

  if (next.size() == m_windows.size()) {
    bool same = true;
    for (const auto& [id, window] : next) {
      const auto oldIt = m_windows.find(id);
      if (oldIt == m_windows.end()
          || oldIt->second.workspaceIndex != window.workspaceIndex
          || oldIt->second.output != window.output
          || oldIt->second.appId != window.appId
          || oldIt->second.title != window.title
          || oldIt->second.x != window.x
          || oldIt->second.y != window.y) {
        same = false;
        break;
      }
    }
    if (same) {
      return false;
    }
  }

  m_windows = std::move(next);
  return true;
}

bool TriadWorkspaceBackend::applyWindow(const nlohmann::json& window) {
  const auto parsed = parseWindow(window);
  if (!parsed.has_value()) {
    return false;
  }
  const auto oldIt = m_windows.find(parsed->id);
  if (oldIt != m_windows.end()
      && oldIt->second.workspaceIndex == parsed->workspaceIndex
      && oldIt->second.output == parsed->output
      && oldIt->second.appId == parsed->appId
      && oldIt->second.title == parsed->title
      && oldIt->second.x == parsed->x
      && oldIt->second.y == parsed->y) {
    return false;
  }
  m_windows[parsed->id] = *parsed;
  return true;
}

std::optional<TriadWorkspaceBackend::WorkspaceState> TriadWorkspaceBackend::parseWorkspace(const nlohmann::json& json) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  const auto indexIt = json.find("workspace_idx");
  if (indexIt == json.end()) {
    return std::nullopt;
  }
  const auto index = jsonUnsigned(*indexIt);
  if (!index.has_value() || *index == 0 || *index > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }

  WorkspaceState workspace;
  workspace.index = static_cast<std::uint32_t>(*index);
  if (const auto tagIdIt = json.find("tag_id"); tagIdIt != json.end()) {
    workspace.tagId = jsonUnsigned(*tagIdIt).value_or(0);
  }
  workspace.name = jsonString(json, "name");
  workspace.output = jsonString(json, "output");
  workspace.active = jsonBool(json, "is_output_visible");
  workspace.globalActive = jsonBool(json, "is_active");
  workspace.urgent = jsonBool(json, "is_urgent");
  workspace.occupied = jsonBool(json, "occupied");
  if (const auto focusedIt = json.find("focused_window_id"); focusedIt != json.end() && !focusedIt->is_null()) {
    workspace.focusedWindowId = jsonUnsigned(*focusedIt);
  }
  return workspace;
}

std::optional<TriadWorkspaceBackend::WindowState> TriadWorkspaceBackend::parseWindow(const nlohmann::json& json) {
  if (!json.is_object()) {
    return std::nullopt;
  }
  const auto idIt = json.find("id");
  const auto workspaceIt = json.find("workspace_idx");
  if (idIt == json.end() || workspaceIt == json.end()) {
    return std::nullopt;
  }
  const auto id = jsonUnsigned(*idIt);
  const auto workspaceIndex = jsonUnsigned(*workspaceIt);
  if (!id.has_value() || !workspaceIndex.has_value() || *workspaceIndex > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }

  WindowState window;
  window.id = *id;
  window.workspaceIndex = static_cast<std::uint32_t>(*workspaceIndex);
  window.output = jsonString(json, "output");
  window.appId = jsonString(json, "app_id");
  window.title = StringUtils::windowTitleSingleLine(jsonString(json, "title"));
  if (const auto* position = objectField(json, "position"); position != nullptr) {
    if (const auto column = position->find("column_idx"); column != position->end()) {
      window.x = jsonInt32(*column).value_or(0);
    }
    if (const auto row = position->find("window_idx"); row != position->end()) {
      window.y = jsonInt32(*row).value_or(0);
    }
  }
  return window;
}

std::string TriadWorkspaceBackend::workspaceKey(const WorkspaceState& workspace) {
  return workspace.index > 0 ? std::to_string(workspace.index) : std::string{};
}

bool TriadWorkspaceBackend::isSyntheticPlaceholder(const WorkspaceState& workspace) {
  return workspace.output.starts_with("triad-") && !workspace.active && !workspace.occupied && !workspace.urgent;
}

std::optional<std::uint64_t> TriadWorkspaceBackend::jsonUnsigned(const nlohmann::json& json) {
  if (json.is_number_unsigned()) {
    return json.get<std::uint64_t>();
  }
  if (json.is_number_integer()) {
    const auto value = json.get<std::int64_t>();
    if (value >= 0) {
      return static_cast<std::uint64_t>(value);
    }
  }
  return std::nullopt;
}

std::string TriadWorkspaceBackend::jsonString(const nlohmann::json& json, const char* key) {
  if (!json.is_object()) {
    return {};
  }
  const auto it = json.find(key);
  return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool TriadWorkspaceBackend::jsonBool(const nlohmann::json& json, const char* key) {
  if (!json.is_object()) {
    return false;
  }
  const auto it = json.find(key);
  return it != json.end() && it->is_boolean() && it->get<bool>();
}

std::string TriadWorkspaceBackend::outputNameFor(wl_output* output) const {
  return output != nullptr && m_outputNameResolver ? m_outputNameResolver(output) : std::string{};
}

bool TriadWorkspaceBackend::shouldExposeWorkspace(
    const WorkspaceState& workspace, const std::string& outputName
) const {
  if (!outputName.empty()) {
    return workspace.output == outputName;
  }
  if (isSyntheticPlaceholder(workspace)) {
    return false;
  }
  if (m_outputNames.empty() || workspace.output.empty()) {
    return true;
  }
  if (m_outputNames.contains(workspace.output)) {
    return true;
  }
  return workspace.active || workspace.globalActive || workspace.occupied || workspace.urgent;
}

std::vector<const TriadWorkspaceBackend::WorkspaceState*>
TriadWorkspaceBackend::sortedWorkspaces(const std::string& outputName) const {
  std::vector<const WorkspaceState*> result;
  result.reserve(m_workspaces.size());
  for (const auto& [index, workspace] : m_workspaces) {
    (void)index;
    if (!shouldExposeWorkspace(workspace, outputName)) {
      continue;
    }
    result.push_back(&workspace);
  }
  std::ranges::sort(result, [](const WorkspaceState* lhs, const WorkspaceState* rhs) {
    if (lhs->index != rhs->index) {
      return lhs->index < rhs->index;
    }
    return lhs->tagId < rhs->tagId;
  });
  return result;
}

std::optional<std::uint32_t> TriadWorkspaceBackend::parseWorkspaceIndex(const std::string& id) const {
  const auto parsed = parseUnsignedId(id);
  if (!parsed.has_value() || *parsed == 0 || *parsed > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*parsed);
}

std::optional<std::uint64_t> TriadWorkspaceBackend::parseUnsignedId(const std::string& id) {
  if (id.empty()) {
    return std::nullopt;
  }

  std::uint64_t parsed = 0;
  const auto [ptr, ec] = std::from_chars(id.data(), id.data() + id.size(), parsed);
  if (ec != std::errc{} || ptr != id.data() + id.size() || parsed == 0) {
    return std::nullopt;
  }
  return parsed;
}

void TriadWorkspaceBackend::refreshSnapshot() {
  const auto response = m_runtime.requestJson("state");
  if (!response.has_value()) {
    return;
  }
  if (const auto* triad = triadObject(*response); triad != nullptr) {
    if (const auto stateIt = triad->find("state"); stateIt != triad->end() && stateIt->is_object()) {
      (void)applyTriadState(*stateIt);
    }
  }
}

void TriadWorkspaceBackend::notifyChanged() const {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void TriadWorkspaceBackend::notifyOverviewChanged() const {
  if (m_overviewChangeCallback) {
    m_overviewChangeCallback();
  }
}
