#include "compositors/hyprland/hyprland_workspace_backend.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/hyprland_window_id.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_set>

HyprlandWorkspaceBackend::HyprlandWorkspaceBackend(
    OutputNameResolver outputNameResolver, compositors::hyprland::HyprlandRuntime& runtime
)
    : compositors::hyprland::HyprlandEventHandler(runtime), m_outputNameResolver(std::move(outputNameResolver)) {}

void HyprlandWorkspaceBackend::setOutputNameResolver(OutputNameResolver outputNameResolver) {
  m_outputNameResolver = std::move(outputNameResolver);
}
bool HyprlandWorkspaceBackend::connectSocket() {
  if (m_runtime.connectSocket()) {
    refreshSnapshot();
    return true;
  }
  return false;
}

bool HyprlandWorkspaceBackend::isAvailable() const noexcept { return m_runtime.available(); }

void HyprlandWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void HyprlandWorkspaceBackend::activate(const std::string& id) {
  if (id.empty() || !m_runtime.available()) {
    return;
  }

  if (m_runtime.configIsLua()) {
    (void)m_runtime.request(std::format("dispatch hl.dsp.focus({{workspace = {}}})", id));
  } else {
    (void)m_runtime.request(std::format("dispatch workspace {}", id));
  }
}

void HyprlandWorkspaceBackend::activateForOutput(wl_output* /*output*/, const std::string& id) { activate(id); }

void HyprlandWorkspaceBackend::activateForOutput(wl_output* /*output*/, const Workspace& workspace) {
  activate(workspace.id);
}

std::vector<Workspace> HyprlandWorkspaceBackend::all() const {
  std::vector<const WorkspaceState*> ordered;
  ordered.reserve(m_workspaces.size());
  for (const auto& workspace : m_workspaces) {
    if (workspace.id >= 0) {
      ordered.push_back(&workspace);
    }
  }

  std::sort(ordered.begin(), ordered.end(), [](const WorkspaceState* a, const WorkspaceState* b) {
    return a->id < b->id;
  });

  std::vector<Workspace> result;
  result.reserve(ordered.size());
  for (const auto* workspace : ordered) {
    result.push_back(toWorkspace(*workspace));
  }
  return result;
}

std::vector<Workspace> HyprlandWorkspaceBackend::forOutput(wl_output* output) const {
  const std::string outputName = m_outputNameResolver != nullptr ? m_outputNameResolver(output) : std::string{};
  if (outputName.empty()) {
    return {};
  }

  std::vector<const WorkspaceState*> ordered;
  for (const auto& workspace : m_workspaces) {
    if (workspace.monitor == outputName) {
      if (workspace.id >= 0) {
        ordered.push_back(&workspace);
      }
    }
  }

  std::sort(ordered.begin(), ordered.end(), [](const WorkspaceState* a, const WorkspaceState* b) {
    return a->id < b->id;
  });

  std::vector<Workspace> result;
  result.reserve(ordered.size());
  for (const auto* workspace : ordered) {
    result.push_back(toWorkspace(*workspace));
  }
  return result;
}

std::unordered_map<std::string, std::vector<std::string>>
HyprlandWorkspaceBackend::appIdsByWorkspace(wl_output* output) const {
  ensureSnapshotFresh();

  const std::string outputName = m_outputNameResolver != nullptr ? m_outputNameResolver(output) : std::string{};
  const bool filterByOutput = output != nullptr && !outputName.empty();

  std::unordered_map<std::string, std::vector<std::string>> byWorkspace;
  std::unordered_map<int, std::unordered_set<std::string>> seenPerWorkspace;
  for (const auto& [address, toplevel] : m_toplevels) {
    (void)address;
    if (toplevel.appId.empty()) {
      continue;
    }
    if (filterByOutput) {
      bool workspaceOnOutput = false;
      for (const auto& workspace : m_workspaces) {
        if (workspace.id == toplevel.workspaceId && workspace.monitor == outputName) {
          workspaceOnOutput = true;
          break;
        }
      }
      if (!workspaceOnOutput && !m_workspaces.empty()) {
        continue;
      }
    }
    auto& seen = seenPerWorkspace[toplevel.workspaceId];
    if (!seen.insert(toplevel.appId).second) {
      continue;
    }
    byWorkspace[std::to_string(toplevel.workspaceId)].push_back(toplevel.appId);
  }
  return byWorkspace;
}

std::vector<WorkspaceWindow> HyprlandWorkspaceBackend::workspaceWindows(wl_output* output) const {
  ensureSnapshotFresh();

  const std::string outputName = m_outputNameResolver != nullptr ? m_outputNameResolver(output) : std::string{};
  const bool filterByOutput = output != nullptr && !outputName.empty();

  std::unordered_set<int> workspacesOnOutput;
  workspacesOnOutput.reserve(m_workspaces.size());
  for (const auto& workspace : m_workspaces) {
    if (filterByOutput && workspace.monitor != outputName) {
      continue;
    }
    if (workspace.id >= 0) {
      workspacesOnOutput.insert(workspace.id);
    }
  }

  std::vector<WorkspaceWindow> result;
  result.reserve(m_toplevels.size());
  for (const auto& [address, toplevel] : m_toplevels) {
    if (toplevel.appId.empty() || toplevel.workspaceId < 0) {
      continue;
    }
    if (filterByOutput && !m_workspaces.empty()) {
      if (!workspacesOnOutput.contains(toplevel.workspaceId)) {
        continue;
      }
    }
    result.push_back(
        WorkspaceWindow{
            .windowId = compositors::hyprland::formatWindowAddress(address),
            .workspaceKey = std::to_string(toplevel.workspaceId),
            .appId = toplevel.appId,
            .title = toplevel.title,
            .x = toplevel.x,
            .y = toplevel.y,
        }
    );
  }
  std::sort(result.begin(), result.end(), [](const WorkspaceWindow& a, const WorkspaceWindow& b) {
    if (a.workspaceKey != b.workspaceKey) {
      return a.workspaceKey < b.workspaceKey;
    }
    if (a.x != b.x) {
      return a.x < b.x;
    }
    if (a.y != b.y) {
      return a.y < b.y;
    }
    return a.windowId < b.windowId;
  });
  return result;
}

std::optional<std::string> HyprlandWorkspaceBackend::focusedWindowId() const {
  if (m_focusedWindowId.empty()) {
    return std::nullopt;
  }
  return m_focusedWindowId;
}

void HyprlandWorkspaceBackend::focusWindow(const std::string& windowId) {
  if (windowId.empty() || !m_runtime.available()) {
    return;
  }
  std::string target = windowId;
  if (target.rfind("address:", 0) != 0) {
    if (target.rfind("0x", 0) == 0) {
      target = "address:" + target;
    } else {
      target = "address:0x" + target;
    }
  }
  (void)m_runtime.request(std::format("dispatch focuswindow {}", target));
}

void HyprlandWorkspaceBackend::notifyCleanup() {
  m_workspaces.clear();
  m_toplevels.clear();
  m_activeWorkspaceByMonitor.clear();
  m_focusedWindowId.clear();
  m_nextOrdinal = 0;
}

void HyprlandWorkspaceBackend::cleanup() { m_runtime.cleanup(); }

int HyprlandWorkspaceBackend::pollFd() const noexcept { return m_runtime.pollFd(); }

void HyprlandWorkspaceBackend::dispatchPoll(short revents) { m_runtime.dispatchPoll(revents); }

void HyprlandWorkspaceBackend::refreshSnapshot() {
  refreshWorkspaces();
  refreshMonitors();
  refreshClients();
  recomputeWorkspaceFlags();
  notifyChanged();
}

void HyprlandWorkspaceBackend::ensureSnapshotFresh() const {
  auto* self = const_cast<HyprlandWorkspaceBackend*>(this);
  if (!m_runtime.available()) {
    if (!self->connectSocket()) {
      return;
    }
  }

  bool changed = false;
  if (m_toplevels.empty()) {
    self->refreshClients();
    changed = true;
  }
  if (m_workspaces.empty()) {
    self->refreshWorkspaces();
    self->refreshMonitors();
    changed = true;
  }
  if (changed) {
    self->recomputeWorkspaceFlags();
  }
}

void HyprlandWorkspaceBackend::refreshWorkspaces() {
  const auto json = m_runtime.requestJson("j/workspaces");
  if (!json || !json->is_array()) {
    return;
  }

  std::unordered_map<int, std::size_t> ordinalsById;
  std::unordered_map<std::string, std::size_t> ordinalsByName;
  for (const auto& workspace : m_workspaces) {
    if (workspace.id >= 0) {
      ordinalsById[workspace.id] = workspace.ordinal;
    } else if (!workspace.name.empty()) {
      ordinalsByName[workspace.name] = workspace.ordinal;
    }
  }

  std::vector<WorkspaceState> next;
  next.reserve(json->size());
  for (const auto& item : *json) {
    if (!item.is_object()) {
      continue;
    }
    WorkspaceState workspace;
    workspace.id = item.value("id", -1);
    workspace.name = item.value("name", "");
    workspace.monitor = item.value("monitor", "");
    if (workspace.id >= 0) {
      if (const auto it = ordinalsById.find(workspace.id); it != ordinalsById.end()) {
        workspace.ordinal = it->second;
      } else {
        workspace.ordinal = m_nextOrdinal++;
      }
    } else if (const auto it = ordinalsByName.find(workspace.name); it != ordinalsByName.end()) {
      workspace.ordinal = it->second;
    } else {
      workspace.ordinal = m_nextOrdinal++;
    }
    next.push_back(std::move(workspace));
  }

  m_workspaces = std::move(next);
}

void HyprlandWorkspaceBackend::refreshMonitors() {
  const auto json = m_runtime.requestJson("j/monitors");
  if (!json || !json->is_array()) {
    return;
  }

  std::unordered_map<std::string, int> activeByMonitor;
  for (const auto& item : *json) {
    if (!item.is_object()) {
      continue;
    }
    const std::string monitorName = item.value("name", "");
    if (monitorName.empty()) {
      continue;
    }

    const auto activeIt = item.find("activeWorkspace");
    if (activeIt != item.end()) {
      const auto idIt = activeIt->find("id");
      if (idIt != activeIt->end() && idIt->is_number_integer()) {
        activeByMonitor[monitorName] = idIt->get<int>();
      }
    }
  }

  if (!activeByMonitor.empty()) {
    m_activeWorkspaceByMonitor = std::move(activeByMonitor);
  }
}

void HyprlandWorkspaceBackend::refreshClients() {
  const auto json = m_runtime.requestJson("j/clients");
  if (!json || !json->is_array()) {
    return;
  }

  std::unordered_map<std::uint64_t, ToplevelState> next;
  next.reserve(json->size());

  for (const auto& item : *json) {
    if (!item.is_object()) {
      continue;
    }

    std::string addressStr;
    if (const auto it = item.find("address"); it != item.end()) {
      if (it->is_string()) {
        addressStr = it->get<std::string>();
      } else if (it->is_number_unsigned()) {
        addressStr = std::format("{:x}", it->get<std::uint64_t>());
      }
    }

    const auto address = parseHexAddress(addressStr);
    if (!address.has_value()) {
      continue;
    }

    ToplevelState state;
    if (const auto wsIt = item.find("workspace"); wsIt != item.end() && wsIt->is_object()) {
      state.workspaceId = wsIt->value("id", -1);
    } else {
      state.workspaceId = -1;
    }
    state.appId = item.value("class", "");
    if (state.appId.empty()) {
      state.appId = item.value("initialClass", "");
    }
    state.title = StringUtils::windowTitleSingleLine(item.value("title", ""));
    if (const auto atIt = item.find("at"); atIt != item.end() && atIt->is_array() && atIt->size() >= 2) {
      state.x = (*atIt)[0].get<std::int32_t>();
      state.y = (*atIt)[1].get<std::int32_t>();
    }

    bool urgent = false;
    bool urgentSet = false;
    if (const auto urgentIt = item.find("urgent"); urgentIt != item.end() && urgentIt->is_boolean()) {
      urgent = urgentIt->get<bool>();
      urgentSet = true;
    }

    if (!urgentSet) {
      if (const auto existing = m_toplevels.find(*address); existing != m_toplevels.end()) {
        urgent = existing->second.urgent;
      }
    }

    state.urgent = urgent;
    next.emplace(*address, std::move(state));

    if (item.value("focused", false)) {
      m_focusedWindowId = compositors::hyprland::formatWindowAddress(*address);
    }
  }

  m_toplevels = std::move(next);
}

void HyprlandWorkspaceBackend::recomputeWorkspaceFlags() {
  std::unordered_map<int, std::size_t> occupiedCounts;
  std::unordered_set<int> urgentByWorkspace;

  for (const auto& [_, toplevel] : m_toplevels) {
    ++occupiedCounts[toplevel.workspaceId];
    if (toplevel.urgent) {
      urgentByWorkspace.insert(toplevel.workspaceId);
    }
  }

  for (auto& workspace : m_workspaces) {
    auto occIt = occupiedCounts.find(workspace.id);
    workspace.occupied = occIt != occupiedCounts.end() && occIt->second > 0;
    workspace.urgent = urgentByWorkspace.contains(workspace.id);
    if (!workspace.monitor.empty()) {
      const auto activeIt = m_activeWorkspaceByMonitor.find(workspace.monitor);
      workspace.active = activeIt != m_activeWorkspaceByMonitor.end() && activeIt->second == workspace.id;
    } else {
      workspace.active = false;
    }
  }
}

void HyprlandWorkspaceBackend::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void HyprlandWorkspaceBackend::syncFromCompositor() { refreshSnapshot(); }

void HyprlandWorkspaceBackend::handleEvent(std::string_view event, std::string_view data) {

  if (event == "configreloaded") {
    refreshSnapshot();
    return;
  }

  if (event == "activewindowv2") {
    const auto args = parseEventArgs(data, 3);
    const auto address = parseHexAddress(args[0]);
    if (!address.has_value() || *address == 0) {
      if (!m_focusedWindowId.empty()) {
        m_focusedWindowId.clear();
        notifyChanged();
      }
      return;
    }
    const auto nextId = compositors::hyprland::formatWindowAddress(*address);
    m_focusedWindowId = nextId;
    // j/clients `at` updates on tile reorder; Hyprland does not expose that via foreign-toplevel protocols.
    refreshClients();
    recomputeWorkspaceFlags();
    notifyChanged();
    return;
  }

  if (event == "focusedmonv2") {
    const auto args = parseEventArgs(data, 2);
    const auto id = parseInt(args[1]);
    if (!id.has_value()) {
      return;
    }
    handleFocusedMonitor(args[0], *id);
    return;
  }

  if (event == "workspacev2") {
    const auto args = parseEventArgs(data, 2);
    const auto id = parseInt(args[0]);
    if (!id.has_value()) {
      return;
    }
    handleWorkspaceActivated(*id);
    return;
  }

  if (event == "createworkspacev2") {
    const auto args = parseEventArgs(data, 2);
    const auto id = parseInt(args[0]);
    const std::string name(args[1]);
    if (!id.has_value() || name.empty()) {
      return;
    }
    auto* workspace = findWorkspaceById(*id);
    if (workspace == nullptr) {
      WorkspaceState state;
      state.id = *id;
      state.name = name;
      state.ordinal = m_nextOrdinal++;
      m_workspaces.push_back(std::move(state));
    } else {
      workspace->name = name;
    }
    refreshWorkspaces();
    recomputeWorkspaceFlags();
    notifyChanged();
    return;
  }

  if (event == "destroyworkspacev2") {
    const auto args = parseEventArgs(data, 2);
    const auto id = parseInt(args[0]);
    const std::string name(args[1]);
    if (!id.has_value()) {
      return;
    }
    m_workspaces.erase(
        std::remove_if(
            m_workspaces.begin(), m_workspaces.end(), [&](const WorkspaceState& ws) { return (ws.id == *id); }
        ),
        m_workspaces.end()
    );

    for (auto it = m_toplevels.begin(); it != m_toplevels.end();) {
      if (it->second.workspaceId == *id) {
        it = m_toplevels.erase(it);
      } else {
        ++it;
      }
    }

    recomputeWorkspaceFlags();
    notifyChanged();
    return;
  }

  if (event == "renameworkspace") {
    const auto args = parseEventArgs(data, 2);
    const auto id = parseInt(args[0]);
    const std::string newName(args[1]);
    if (!id.has_value() || newName.empty()) {
      return;
    }
    auto* workspace = findWorkspaceById(*id);
    if (workspace == nullptr) {
      refreshWorkspaces();
      recomputeWorkspaceFlags();
      notifyChanged();
      return;
    }
    const std::string oldName = workspace->name;
    workspace->name = newName;

    recomputeWorkspaceFlags();
    notifyChanged();
    return;
  }

  if (event == "moveworkspacev2") {
    const auto args = parseEventArgs(data, 3);
    const auto id = parseInt(args[0]);
    const std::string name(args[1]);
    const std::string monitor(args[2]);
    if (!id.has_value()) {
      return;
    }
    auto* workspace = findWorkspaceById(*id);
    if (workspace != nullptr && !monitor.empty()) {
      workspace->monitor = monitor;
      recomputeWorkspaceFlags();
      notifyChanged();
    }
    return;
  }

  if (event == "openwindow") {
    const auto args = parseEventArgs(data, 4);
    const auto address = parseHexAddress(args[0]);
    const auto workspaceName = args[1];

    if (!address.has_value() || workspaceName.empty()) {
      return;
    }
    const auto workspace = findWorkspaceByName(workspaceName);
    if (workspace == nullptr) {
      refreshClients();
      recomputeWorkspaceFlags();
      notifyChanged();
      return;
    }
    moveToplevel(*address, workspace->id);
    if (auto it = m_toplevels.find(*address); it != m_toplevels.end()) {
      it->second.appId = std::string(args[2]);
      it->second.title = StringUtils::windowTitleSingleLine(args[3]);
    }
    refreshClients();
    recomputeWorkspaceFlags();
    notifyChanged();
    return;
  }

  if (event == "closewindow") {
    const auto args = parseEventArgs(data, 1);
    const auto address = parseHexAddress(args[0]);
    if (!address.has_value()) {
      return;
    }
    m_toplevels.erase(*address);
    recomputeWorkspaceFlags();
    notifyChanged();
    return;
  }

  if (event == "movewindowv2") {
    const auto args = parseEventArgs(data, 3);
    const auto address = parseHexAddress(args[0]);
    const auto id = parseInt(args[2]);
    if (!address.has_value() || !id.has_value()) {
      return;
    }
    moveToplevel(*address, *id);
    refreshClients();
    recomputeWorkspaceFlags();
    notifyChanged();
    return;
  }

  if (event == "urgent") {
    const auto args = parseEventArgs(data, 1);
    const auto address = parseHexAddress(args[0]);
    if (!address.has_value()) {
      return;
    }
    auto it = m_toplevels.find(*address);
    if (it == m_toplevels.end()) {
      m_toplevels.emplace(*address, ToplevelState{.workspaceId = -1, .appId = {}, .title = {}, .urgent = true});
      refreshClients();
    } else {
      it->second.urgent = true;
    }
    recomputeWorkspaceFlags();
    notifyChanged();
  }
}

void HyprlandWorkspaceBackend::handleFocusedMonitor(std::string_view monitorName, int workspaceId) {
  if (monitorName.empty()) {
    return;
  }
  m_activeWorkspaceByMonitor[std::string(monitorName)] = workspaceId;
  clearUrgentForWorkspace(workspaceId);
  recomputeWorkspaceFlags();
  notifyChanged();
}

void HyprlandWorkspaceBackend::handleWorkspaceActivated(int workspaceId) {
  refreshMonitors();
  clearUrgentForWorkspace(workspaceId);
  recomputeWorkspaceFlags();
  notifyChanged();
}

void HyprlandWorkspaceBackend::clearUrgentForWorkspace(int workspaceId) {
  for (auto& [_, toplevel] : m_toplevels) {
    if (toplevel.workspaceId == workspaceId) {
      toplevel.urgent = false;
    }
  }
}

void HyprlandWorkspaceBackend::moveToplevel(std::uint64_t address, int workspaceId) {
  auto& toplevel = m_toplevels[address];
  toplevel.workspaceId = workspaceId;
}

HyprlandWorkspaceBackend::WorkspaceState* HyprlandWorkspaceBackend::findWorkspaceById(int id) {
  for (auto& workspace : m_workspaces) {
    if (workspace.id == id) {
      return &workspace;
    }
  }
  return nullptr;
}

HyprlandWorkspaceBackend::WorkspaceState* HyprlandWorkspaceBackend::findWorkspaceByName(std::string_view name) {
  if (name.empty()) {
    return nullptr;
  }
  for (auto& workspace : m_workspaces) {
    if (workspace.name == name) {
      return &workspace;
    }
  }
  return nullptr;
}

std::optional<std::uint64_t> HyprlandWorkspaceBackend::parseHexAddress(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  if (value.starts_with("0x") || value.starts_with("0X")) {
    value = value.substr(2);
  }
  std::uint64_t address = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(begin, end, address, 16);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return address;
}

std::optional<int> HyprlandWorkspaceBackend::parseInt(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  int parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

std::vector<std::string_view> HyprlandWorkspaceBackend::parseEventArgs(std::string_view data, std::size_t count) {
  std::vector<std::string_view> args;
  args.reserve(count);
  std::size_t start = 0;
  for (std::size_t i = 0; i + 1 < count; ++i) {
    const auto split = data.find(',', start);
    if (split == std::string_view::npos) {
      break;
    }
    args.push_back(data.substr(start, split - start));
    start = split + 1;
  }
  if (start <= data.size()) {
    args.push_back(data.substr(start));
  }
  while (args.size() < count) {
    args.push_back({});
  }
  return args;
}

Workspace HyprlandWorkspaceBackend::toWorkspace(const WorkspaceState& state) {
  const std::uint32_t coord =
      state.id >= 0 ? static_cast<std::uint32_t>(state.id - 1) : static_cast<std::uint32_t>(state.ordinal);
  return Workspace{
      .id = std::to_string(state.id),
      .name = state.name,
      .coordinates = {coord},
      .active = state.active,
      .urgent = state.urgent,
      .occupied = state.occupied,
  };
}
