#include "compositors/wayfire/wayfire_workspace_backend.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_set>

namespace {

  constexpr Logger kLog("workspace_wayfire");

  // Messages are a 4-byte little-endian length followed by that many bytes of JSON.
  constexpr std::size_t kHeaderSize = 4;
  constexpr std::size_t kMaxMessageSize = 64U * 1024U * 1024U;

  // Everything that can move a window to another workspace, or add, remove or rename one.
  constexpr std::string_view kWatchedEvents = R"({"events":["view-mapped","view-unmapped","view-geometry-changed",)"
                                              R"("view-set-output","view-workspace-changed","view-wset-changed",)"
                                              R"("view-sticky","view-title-changed","view-app-id-changed",)"
                                              R"("wset-workspace-changed","output-wset-changed","output-added",)"
                                              R"("output-removed","output-layout-changed"]})";

  std::string keyFor(const Workspace& workspace) { return !workspace.name.empty() ? workspace.name : workspace.id; }

  // Wayfire's own ext-workspace plugin names each workspace "<output>:workspace-<column>-<row>"
  // and gives (column, row) coordinates; other providers, such as the ext-workspace-bridge
  // plugin, give (row, column).
  bool upstreamCell(std::string_view name, int& column, int& row) {
    constexpr std::string_view kMarker = ":workspace-";
    const auto marker = name.rfind(kMarker);
    if (marker == std::string_view::npos) {
      return false;
    }
    const char* begin = name.data() + marker + kMarker.size();
    const char* end = name.data() + name.size();
    auto [afterColumn, columnError] = std::from_chars(begin, end, column);
    if (columnError != std::errc{} || afterColumn == end || *afterColumn != '-') {
      return false;
    }
    auto [afterRow, rowError] = std::from_chars(afterColumn + 1, end, row);
    return rowError == std::errc{} && afterRow == end;
  }

  double number(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<double>() : 0.0;
  }

  std::string text(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
  }

} // namespace

WayfireWorkspaceBackend::WayfireWorkspaceBackend() { connectSocket(); }

WayfireWorkspaceBackend::~WayfireWorkspaceBackend() { cleanup(); }

void WayfireWorkspaceBackend::setProviders(WorkspacesProvider workspaces, ToplevelsProvider toplevels) {
  m_workspacesProvider = std::move(workspaces);
  m_toplevelsProvider = std::move(toplevels);
}

void WayfireWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void WayfireWorkspaceBackend::cleanup() {
  if (m_socketFd >= 0) {
    ::close(m_socketFd);
    m_socketFd = -1;
  }
  m_readBuffer.clear();
  m_pendingReplies.clear();
  m_refreshQueued = false;
  m_outputs.clear();
  m_views.clear();
}

void WayfireWorkspaceBackend::connectSocket() {
  const char* path = std::getenv("WAYFIRE_SOCKET");
  if (path == nullptr || path[0] == '\0') {
    kLog.info("WAYFIRE_SOCKET is not set; taskbar workspace grouping is unavailable");
    return;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (std::strlen(path) >= sizeof(addr.sun_path)) {
    kLog.warn("Wayfire IPC socket path too long");
    return;
  }
  std::memcpy(addr.sun_path, path, std::strlen(path) + 1);

  m_socketFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (m_socketFd < 0) {
    kLog.warn("failed to create Wayfire IPC socket: {}", std::strerror(errno));
    return;
  }
  if (::connect(m_socketFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    kLog.warn("failed to connect to Wayfire IPC {} (is the ipc plugin enabled?): {}", path, std::strerror(errno));
    cleanup();
    return;
  }

  send("window-rules/events/watch", std::string(kWatchedEvents));
  m_pendingReplies.push_back(Reply::Watch);
  requestRefresh();
}

void WayfireWorkspaceBackend::send(const std::string& method, const std::string& data) {
  if (m_socketFd < 0) {
    return;
  }
  const std::string payload = R"({"method":")" + method + R"(","data":)" + data + "}";
  const auto length = static_cast<std::uint32_t>(payload.size());
  std::string message;
  message.reserve(kHeaderSize + payload.size());
  for (std::size_t i = 0; i < kHeaderSize; ++i) {
    message.push_back(static_cast<char>((length >> (8U * i)) & 0xFFU));
  }
  message += payload;

  std::size_t offset = 0;
  while (offset < message.size()) {
    const ssize_t written = ::send(m_socketFd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      kLog.warn("failed to send Wayfire IPC request: {}", std::strerror(errno));
      cleanup();
      return;
    }
    offset += static_cast<std::size_t>(written);
  }
}

void WayfireWorkspaceBackend::requestRefresh() {
  if (m_socketFd < 0) {
    return;
  }
  if (std::ranges::find(m_pendingReplies, Reply::Views) != m_pendingReplies.end()) {
    m_refreshQueued = true;
    return;
  }
  send("window-rules/list-outputs");
  m_pendingReplies.push_back(Reply::Outputs);
  send("window-rules/list-views");
  m_pendingReplies.push_back(Reply::Views);
}

void WayfireWorkspaceBackend::dispatchPoll(short revents) {
  if (m_socketFd < 0) {
    return;
  }
  if ((revents & (POLLHUP | POLLERR)) != 0 && (revents & POLLIN) == 0) {
    kLog.warn("Wayfire IPC connection lost");
    cleanup();
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }
  if ((revents & POLLIN) != 0) {
    readSocket();
  }
}

void WayfireWorkspaceBackend::readSocket() {
  char buffer[16384];
  while (m_socketFd >= 0) {
    const ssize_t n = ::recv(m_socketFd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (n > 0) {
      m_readBuffer.insert(m_readBuffer.end(), buffer, buffer + n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    kLog.warn("Wayfire IPC closed the connection");
    cleanup();
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }

  while (m_readBuffer.size() >= kHeaderSize) {
    std::uint32_t length = 0;
    for (std::size_t i = 0; i < kHeaderSize; ++i) {
      length |= static_cast<std::uint32_t>(static_cast<unsigned char>(m_readBuffer[i])) << (8U * i);
    }
    if (length > kMaxMessageSize) {
      kLog.warn("oversized Wayfire IPC message ({} bytes); disconnecting", length);
      cleanup();
      return;
    }
    if (m_readBuffer.size() < kHeaderSize + length) {
      break;
    }
    std::string payload(m_readBuffer.begin() + kHeaderSize, m_readBuffer.begin() + kHeaderSize + length);
    m_readBuffer.erase(m_readBuffer.begin(), m_readBuffer.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + length));
    handleMessage(payload);
    if (m_socketFd < 0) {
      return;
    }
  }
}

void WayfireWorkspaceBackend::handleMessage(const std::string& payload) {
  // Events carry an "event" member; everything else answers the oldest outstanding request.
  const auto json = nlohmann::json::parse(payload, nullptr, false);
  if (json.is_object() && json.contains("event")) {
    requestRefresh();
    return;
  }
  if (m_pendingReplies.empty()) {
    return;
  }
  const Reply reply = m_pendingReplies.front();
  m_pendingReplies.pop_front();
  switch (reply) {
  case Reply::Watch:
    if (json.is_object() && json.contains("error")) {
      kLog.warn("Wayfire IPC refused the event watch: {}", text(json, "error"));
    }
    return;
  case Reply::Outputs:
    parseOutputs(payload);
    return;
  case Reply::Views:
    parseViews(payload);
    if (m_changeCallback) {
      m_changeCallback();
    }
    if (m_refreshQueued) {
      m_refreshQueued = false;
      requestRefresh();
    }
    return;
  }
}

void WayfireWorkspaceBackend::parseOutputs(const std::string& payload) {
  const auto json = nlohmann::json::parse(payload, nullptr, false);
  if (!json.is_array()) {
    return;
  }
  m_outputs.clear();
  for (const auto& output : json) {
    if (!output.is_object() || !output.contains("geometry") || !output.contains("workspace")) {
      continue;
    }
    const auto& geometry = output["geometry"];
    const auto& workspace = output["workspace"];
    m_outputs[text(output, "name")] = OutputState{
        .width = number(geometry, "width"),
        .height = number(geometry, "height"),
        .workspaceX = static_cast<int>(number(workspace, "x")),
        .workspaceY = static_cast<int>(number(workspace, "y")),
        .gridWidth = std::max(1, static_cast<int>(number(workspace, "grid_width"))),
        .gridHeight = std::max(1, static_cast<int>(number(workspace, "grid_height"))),
    };
  }
}

void WayfireWorkspaceBackend::parseViews(const std::string& payload) {
  const auto json = nlohmann::json::parse(payload, nullptr, false);
  if (!json.is_array()) {
    return;
  }
  m_views.clear();
  for (const auto& view : json) {
    if (!view.is_object() || text(view, "role") != "toplevel") {
      continue;
    }
    if (const auto mapped = view.find("mapped"); mapped != view.end() && mapped->is_boolean() && !mapped->get<bool>()) {
      continue;
    }
    const auto geometry = view.contains("geometry") ? view["geometry"] : nlohmann::json::object();
    const auto sticky = view.find("sticky");
    m_views.push_back(
        ViewState{
            .id = static_cast<std::int64_t>(number(view, "id")),
            .appId = text(view, "app-id"),
            .title = text(view, "title"),
            .outputName = text(view, "output-name"),
            .x = number(geometry, "x"),
            .y = number(geometry, "y"),
            .width = number(geometry, "width"),
            .height = number(geometry, "height"),
            .sticky = sticky != view.end() && sticky->is_boolean() && sticky->get<bool>(),
        }
    );
  }
  std::ranges::sort(m_views, {}, &ViewState::id);
}

bool WayfireWorkspaceBackend::cellForView(const ViewState& view, int& column, int& row) const {
  const auto it = m_outputs.find(view.outputName);
  if (it == m_outputs.end() || it->second.width <= 0.0 || it->second.height <= 0.0) {
    return false;
  }
  const OutputState& output = it->second;
  column = output.workspaceX;
  row = output.workspaceY;
  if (!view.sticky) {
    // Geometry is relative to the output's current workspace; whole output widths away is
    // another workspace.
    column += static_cast<int>(std::floor((view.x + view.width / 2.0) / output.width));
    row += static_cast<int>(std::floor((view.y + view.height / 2.0) / output.height));
  }
  column = std::clamp(column, 0, output.gridWidth - 1);
  row = std::clamp(row, 0, output.gridHeight - 1);
  return true;
}

std::string WayfireWorkspaceBackend::workspaceKeyAt(const std::vector<Workspace>& workspaces, int column, int row) {
  for (const auto& workspace : workspaces) {
    int workspaceColumn = 0;
    int workspaceRow = 0;
    if (!upstreamCell(keyFor(workspace), workspaceColumn, workspaceRow)) {
      if (workspace.coordinates.size() < 2) {
        continue;
      }
      workspaceRow = static_cast<int>(workspace.coordinates[0]);
      workspaceColumn = static_cast<int>(workspace.coordinates[1]);
    }
    if (workspaceColumn == column && workspaceRow == row) {
      return keyFor(workspace);
    }
  }
  return {};
}

std::vector<WorkspaceWindow> WayfireWorkspaceBackend::workspaceWindows(const std::string& outputName) const {
  if (m_views.empty() || !m_workspacesProvider || !m_toplevelsProvider) {
    return {};
  }

  // Wayfire's view ids and the wlr handles both follow the order windows appeared in, so the Nth
  // window with a given app id and title in one list is the Nth in the other. Groups whose sizes
  // differ are mid-update and get no workspace until the next refresh.
  std::unordered_map<std::string, std::vector<WlrToplevelSnapshot>> handlesByIdentity;
  m_toplevelsProvider([&](const WlrToplevelSnapshot& toplevel) {
    if (toplevel.handle != nullptr) {
      handlesByIdentity[toplevel.appId + '\n' + toplevel.title].push_back(toplevel);
    }
  });
  std::unordered_map<std::string, std::vector<const ViewState*>> viewsByIdentity;
  for (const auto& view : m_views) {
    viewsByIdentity[view.appId + '\n' + view.title].push_back(&view);
  }

  std::unordered_map<std::string, std::vector<Workspace>> workspacesByOutput;
  struct Entry {
    std::uint64_t order = 0;
    WorkspaceWindow window;
  };
  std::vector<Entry> entries;
  for (auto& [identity, handles] : handlesByIdentity) {
    const auto views = viewsByIdentity.find(identity);
    if (views == viewsByIdentity.end() || views->second.size() != handles.size()) {
      continue;
    }
    std::ranges::sort(handles, {}, &WlrToplevelSnapshot::order);
    for (std::size_t i = 0; i < handles.size(); ++i) {
      const ViewState& view = *views->second[i];
      if (!outputName.empty() && view.outputName != outputName) {
        continue;
      }
      int column = 0;
      int row = 0;
      if (!cellForView(view, column, row)) {
        continue;
      }
      auto cached = workspacesByOutput.find(view.outputName);
      if (cached == workspacesByOutput.end()) {
        cached = workspacesByOutput.emplace(view.outputName, m_workspacesProvider(view.outputName)).first;
      }
      std::string key = workspaceKeyAt(cached->second, column, row);
      if (key.empty()) {
        continue;
      }
      const WlrToplevelSnapshot& handle = handles[i];
      entries.push_back(
          Entry{
              .order = handle.order,
              .window = WorkspaceWindow{
                  .windowId = toplevel_identity::wlr(reinterpret_cast<std::uintptr_t>(handle.handle)),
                  .workspaceKey = std::move(key),
                  .appId = handle.appId,
                  .title = handle.title,
                  .x = static_cast<std::int32_t>(view.x),
                  .y = static_cast<std::int32_t>(view.y),
                  .outputName = view.outputName,
                  .minimizeState = WindowMinimizeState{.minimized = handle.minimized, .minimizable = true},
              },
          }
      );
    }
  }
  std::ranges::sort(entries, {}, &Entry::order);

  std::vector<WorkspaceWindow> result;
  result.reserve(entries.size());
  for (auto& entry : entries) {
    result.push_back(std::move(entry.window));
  }
  return result;
}

void WayfireWorkspaceBackend::apply(std::vector<Workspace>& workspaces, const std::string& outputName) const {
  if (m_socketFd < 0) {
    return;
  }
  std::unordered_set<std::string> occupied;
  for (const auto& window : workspaceWindows(outputName)) {
    occupied.insert(window.workspaceKey);
  }
  for (auto& workspace : workspaces) {
    workspace.occupied = occupied.contains(keyFor(workspace));
  }
}

std::vector<std::string> WayfireWorkspaceBackend::workspaceKeys(const std::string& outputName) const {
  if (!m_workspacesProvider) {
    return {};
  }
  std::vector<std::string> keys;
  for (const auto& workspace : m_workspacesProvider(outputName)) {
    keys.push_back(keyFor(workspace));
  }
  return keys;
}

std::unordered_map<std::string, std::vector<std::string>>
WayfireWorkspaceBackend::appIdsByWorkspace(const std::string& outputName) const {
  std::unordered_map<std::string, std::vector<std::string>> grouped;
  for (const auto& window : workspaceWindows(outputName)) {
    auto& apps = grouped[window.workspaceKey];
    if (!window.appId.empty() && std::ranges::find(apps, window.appId) == apps.end()) {
      apps.push_back(window.appId);
    }
  }
  return grouped;
}
