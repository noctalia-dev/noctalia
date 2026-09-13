#include "compositors/hyprland/hyprland_workspace_backend.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/hyprland_window_id.h"
#include "core/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>
#include <tuple>
#include <unordered_set>

namespace {

  constexpr Logger kLog("hyprland_workspace");

} // namespace

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

  std::string target = id;
  if (const auto* workspace = findWorkspaceByKey(id); workspace != nullptr) {
    target = workspace->identity.selector;
  } else if (const auto parsed = parseInt(id); parsed.has_value() && *parsed < 0) {
    // Legacy named workspace IDs are not valid absolute dispatcher targets.
    return;
  }

  if (m_runtime.configIsLua()) {
    (void)m_runtime.request(std::format("dispatch hl.dsp.focus({{workspace = \"{}\"}})", target));
  } else {
    (void)m_runtime.request(std::format("dispatch workspace {}", target));
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
    if (!isSpecial(workspace)) {
      ordered.push_back(&workspace);
    }
  }

  std::ranges::sort(ordered, workspaceOrderLess);

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
    if (workspace.monitor == outputName && !isSpecial(workspace)) {
      ordered.push_back(&workspace);
    }
  }

  std::ranges::sort(ordered, workspaceOrderLess);

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
  std::unordered_map<std::string, std::unordered_set<std::string>> seenPerWorkspace;
  for (const auto& [address, toplevel] : m_toplevels) {
    (void)address;
    if (toplevel.appId.empty() || toplevel.workspaceKey.empty()) {
      continue;
    }
    if (filterByOutput) {
      bool workspaceOnOutput = false;
      for (const auto& workspace : m_workspaces) {
        if (workspace.identity.key == toplevel.workspaceKey && workspace.monitor == outputName) {
          workspaceOnOutput = true;
          break;
        }
      }
      if (!workspaceOnOutput && !m_workspaces.empty()) {
        continue;
      }
    }
    auto& seen = seenPerWorkspace[toplevel.workspaceKey];
    if (!seen.insert(toplevel.appId).second) {
      continue;
    }
    byWorkspace[assignmentKeyFor(toplevel.workspaceKey)].push_back(toplevel.appId);
  }
  return byWorkspace;
}

std::vector<WorkspaceWindow> HyprlandWorkspaceBackend::workspaceWindows(wl_output* output) const {
  ensureSnapshotFresh();

  const std::string outputName = m_outputNameResolver != nullptr ? m_outputNameResolver(output) : std::string{};
  const bool filterByOutput = output != nullptr && !outputName.empty();

  std::unordered_set<std::string> workspacesOnOutput;
  workspacesOnOutput.reserve(m_workspaces.size());
  for (const auto& workspace : m_workspaces) {
    if (filterByOutput && workspace.monitor != outputName) {
      continue;
    }
    workspacesOnOutput.insert(workspace.identity.key);
  }

  std::vector<WorkspaceWindow> result;
  result.reserve(m_toplevels.size());
  for (const auto& [address, toplevel] : m_toplevels) {
    if (toplevel.workspaceKey.empty() || (m_ipcSchema == IpcSchema::LegacyId && toplevel.workspaceKey == "-1")) {
      continue;
    }
    if (filterByOutput && !m_workspaces.empty()) {
      if (!workspacesOnOutput.contains(toplevel.workspaceKey)) {
        continue;
      }
    }
    result.push_back(
        WorkspaceWindow{
            .windowId = compositors::hyprland::formatWindowAddress(address),
            .workspaceKey = assignmentKeyFor(toplevel.workspaceKey),
            .appId = toplevel.appId,
            .title = toplevel.title,
            .x = toplevel.x,
            .y = toplevel.y,
            .outputName = {},
        }
    );
  }
  std::ranges::sort(result, [](const WorkspaceWindow& a, const WorkspaceWindow& b) {
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
  if (windowId.empty()) {
    return;
  }
  const auto normalized = compositors::hyprland::normalizeWindowId(windowId);
  if (normalized.empty()) {
    return;
  }
  const std::string target = "address:0x" + normalized;
  if (m_runtime.configIsLua()) {
    (void)m_runtime.request(std::format("dispatch hl.dsp.focus({{window = \"{}\"}})", target));
  } else {
    (void)m_runtime.request(std::format("dispatch focuswindow {}", target));
  }
  (void)m_runtime.request(std::format("dispatch alterzorder top,{}", target));
}

void HyprlandWorkspaceBackend::notifyCleanup() {
  m_workspaces.clear();
  m_toplevels.clear();
  m_activeWorkspaceByMonitor.clear();
  m_focusedWindowId.clear();
  m_nextOrdinal = 0;
  m_ipcSchema = IpcSchema::Unknown;
}

void HyprlandWorkspaceBackend::cleanup() { m_runtime.cleanup(); }

int HyprlandWorkspaceBackend::pollFd() const noexcept { return m_runtime.pollFd(); }

void HyprlandWorkspaceBackend::dispatchPoll(short revents) { m_runtime.dispatchPoll(revents); }

void HyprlandWorkspaceBackend::refreshSnapshot() {
  if (refreshWorkspaces() == WorkspaceRefreshResult::Invalid) {
    return;
  }
  refreshMonitors();
  refreshClients();
  recomputeWorkspaceFlags();
  notifyChanged();
}

void HyprlandWorkspaceBackend::syncFromCompositor() { refreshSnapshot(); }

void HyprlandWorkspaceBackend::reconcileFromCompositor() {
  if (refreshWorkspaces() != WorkspaceRefreshResult::Changed) {
    return;
  }
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
    if (self->refreshWorkspaces() != WorkspaceRefreshResult::Invalid) {
      self->refreshMonitors();
      changed = true;
    }
  }
  if (changed) {
    self->recomputeWorkspaceFlags();
  }
}

HyprlandWorkspaceBackend::WorkspaceRefreshResult HyprlandWorkspaceBackend::refreshWorkspaces() {
  const auto json = m_runtime.requestJson("j/workspaces");
  if (!json || !json->is_array()) {
    return WorkspaceRefreshResult::Invalid;
  }

  IpcSchema schema = m_ipcSchema;
  if (!json->empty()) {
    const auto detectedSchema = detectIpcSchema(*json);
    if (!detectedSchema.has_value()) {
      kLog.warn("rejecting mixed or malformed Hyprland workspace IPC schema");
      return WorkspaceRefreshResult::Invalid;
    }
    schema = *detectedSchema;
  } else if (schema == IpcSchema::Unknown) {
    return WorkspaceRefreshResult::Invalid;
  }
  if (m_ipcSchema != IpcSchema::Unknown && m_ipcSchema != schema) {
    kLog.warn("rejecting Hyprland workspace IPC schema change on an active connection");
    return WorkspaceRefreshResult::Invalid;
  }
  const bool initializingSchema = m_ipcSchema == IpcSchema::Unknown;

  auto ordinalKey = [schema](const WorkspaceState& workspace) {
    if (schema == IpcSchema::LegacyId && workspace.identity.legacyId.value_or(0) < 0) {
      return "name:" + workspace.name;
    }
    return workspace.identity.key;
  };

  std::unordered_map<std::string, std::size_t> ordinals;
  if (!initializingSchema) {
    for (const auto& workspace : m_workspaces) {
      ordinals[ordinalKey(workspace)] = workspace.ordinal;
    }
  }
  std::size_t nextOrdinal = m_nextOrdinal;

  std::vector<WorkspaceState> next;
  next.reserve(json->size());
  for (const auto& item : *json) {
    const auto identity = parseJsonWorkspaceIdentity(item, schema);
    if (!identity.has_value()) {
      kLog.warn("rejecting malformed Hyprland workspace identity");
      return WorkspaceRefreshResult::Invalid;
    }

    WorkspaceState workspace;
    workspace.identity = *identity;
    if (const auto it = item.find("name"); it != item.end() && it->is_string()) {
      workspace.name = it->get<std::string>();
    }
    if (const auto it = item.find("monitor"); it != item.end() && it->is_string()) {
      workspace.monitor = it->get<std::string>();
    }

    const std::string key = ordinalKey(workspace);
    if (const auto it = ordinals.find(key); it != ordinals.end()) {
      workspace.ordinal = it->second;
    } else {
      workspace.ordinal = nextOrdinal++;
    }
    next.push_back(std::move(workspace));
  }

  std::unordered_set<std::string> seenKeys;
  for (const auto& ws : next) {
    if (schema == IpcSchema::LegacyId && ws.identity.legacyId.value_or(0) < 0) {
      seenKeys.insert(ws.name);
    } else {
      seenKeys.insert(ws.identity.key);
    }
  }

  const auto rulesJson = m_runtime.requestJson("j/workspacerules");
  if (rulesJson && rulesJson->is_array()) {
    for (const auto& item : *rulesJson) {
      if (!item.is_object() || !item.value("enabled", true)) {
        continue;
      }

      const std::string workspaceString = item.value("workspaceString", "");
      if (workspaceString.empty()) {
        continue;
      }

      // Only concrete IDs and explicit name: targets identify a workspace; selectors must not create phantom entries.
      const bool namedWorkspace = workspaceString.starts_with("name:");
      const std::string workspaceAddress = namedWorkspace ? workspaceString.substr(5) : std::string{};
      const auto legacyNumber =
          schema == IpcSchema::LegacyId && !namedWorkspace ? parseInt(workspaceString) : std::optional<int>{};
      const auto stableNumber = schema == IpcSchema::StableIdentity && !namedWorkspace ? parseUnsigned(workspaceString)
                                                                                       : std::optional<std::uint32_t>{};
      const bool numberedWorkspace = legacyNumber.has_value() ? *legacyNumber > 0 : stableNumber.has_value();
      if ((namedWorkspace && workspaceAddress.empty()) || (!namedWorkspace && !numberedWorkspace)) {
        continue;
      }

      WorkspaceIdentity identity;
      if (numberedWorkspace) {
        const auto number = schema == IpcSchema::LegacyId ? static_cast<std::uint32_t>(*legacyNumber) : *stableNumber;
        identity.key = std::to_string(number);
        identity.selector = identity.key;
        identity.address = identity.key;
        identity.kind = WorkspaceKind::Numbered;
        identity.number = number;
        if (schema == IpcSchema::LegacyId) {
          identity.legacyId = legacyNumber;
        }
      } else {
        identity.key = schema == IpcSchema::LegacyId ? "-1" : "name:" + workspaceAddress;
        identity.selector = "name:" + workspaceAddress;
        identity.address = workspaceAddress;
        identity.kind = WorkspaceKind::Named;
        if (schema == IpcSchema::LegacyId) {
          identity.legacyId = -1;
        }
      }

      const std::string key = schema == IpcSchema::LegacyId && namedWorkspace ? workspaceAddress : identity.key;
      const std::string defaultName = item.value("defaultName", "");
      if (numberedWorkspace && !defaultName.empty()) {
        const auto existing = std::ranges::find(next, identity.key, [](const WorkspaceState& workspace) {
          return workspace.identity.key;
        });
        if (existing != next.end() && (existing->name.empty() || existing->name == key)) {
          existing->name = defaultName;
        }
      }

      const bool isPersistent = item.value("persistent", false);
      if (!isPersistent || seenKeys.contains(key)) {
        continue;
      }

      WorkspaceState workspace;
      workspace.identity = std::move(identity);
      workspace.name = namedWorkspace
          ? (schema == IpcSchema::StableIdentity && !defaultName.empty() ? defaultName : workspaceAddress)
          : defaultName;
      workspace.monitor = item.value("monitor", "");

      const std::string persistentOrdinalKey = ordinalKey(workspace);
      if (const auto it = ordinals.find(persistentOrdinalKey); it != ordinals.end()) {
        workspace.ordinal = it->second;
      } else {
        workspace.ordinal = nextOrdinal++;
      }

      seenKeys.insert(key);
      next.push_back(std::move(workspace));
    }
  }

  const bool changed = [&] {
    if (initializingSchema) {
      return true;
    }
    if (next.size() != m_workspaces.size()) {
      return true;
    }
    std::vector<std::tuple<std::string, std::string, std::string, std::size_t>> before;
    std::vector<std::tuple<std::string, std::string, std::string, std::size_t>> after;
    before.reserve(m_workspaces.size());
    after.reserve(next.size());
    for (const auto& workspace : m_workspaces) {
      before.emplace_back(workspace.identity.key, workspace.name, workspace.monitor, workspace.ordinal);
    }
    for (const auto& workspace : next) {
      after.emplace_back(workspace.identity.key, workspace.name, workspace.monitor, workspace.ordinal);
    }
    std::ranges::sort(before);
    std::ranges::sort(after);
    return before != after;
  }();

  if (!changed) {
    return WorkspaceRefreshResult::Unchanged;
  }

  m_ipcSchema = schema;
  m_nextOrdinal = nextOrdinal;
  m_workspaces = std::move(next);
  return WorkspaceRefreshResult::Changed;
}

void HyprlandWorkspaceBackend::refreshMonitors() {
  if (m_ipcSchema == IpcSchema::Unknown) {
    return;
  }
  const auto json = m_runtime.requestJson("j/monitors");
  if (!json || !json->is_array()) {
    return;
  }

  std::unordered_map<std::string, std::string> activeByMonitor;
  for (const auto& item : *json) {
    if (!item.is_object()) {
      continue;
    }
    const std::string monitorName = item.value("name", "");
    if (monitorName.empty()) {
      continue;
    }

    const auto activeIt = item.find("activeWorkspace");
    if (activeIt != item.end() && activeIt->is_object()) {
      const auto identity = parseJsonWorkspaceIdentity(*activeIt, m_ipcSchema);
      if (!identity.has_value()) {
        kLog.warn("rejecting malformed Hyprland monitor workspace identity");
        return;
      }
      activeByMonitor[monitorName] = identity->key;
    }
  }

  m_activeWorkspaceByMonitor = std::move(activeByMonitor);
}

void HyprlandWorkspaceBackend::refreshClients() {
  if (m_ipcSchema == IpcSchema::Unknown) {
    return;
  }
  const auto json = m_runtime.requestJson("j/clients");
  if (!json || !json->is_array()) {
    return;
  }

  std::unordered_map<std::uint64_t, ToplevelState> next;
  next.reserve(json->size());
  std::string nextFocusedWindowId = m_focusedWindowId;

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
      const auto identity = parseJsonWorkspaceIdentity(*wsIt, m_ipcSchema);
      if (!identity.has_value()) {
        kLog.warn("rejecting malformed Hyprland client workspace identity");
        return;
      }
      state.workspaceKey = identity->key;
    } else {
      state.workspaceKey = m_ipcSchema == IpcSchema::LegacyId ? "-1" : std::string{};
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
      nextFocusedWindowId = compositors::hyprland::formatWindowAddress(*address);
    }
  }

  m_toplevels = std::move(next);
  m_focusedWindowId = std::move(nextFocusedWindowId);
}

void HyprlandWorkspaceBackend::recomputeWorkspaceFlags() {
  std::unordered_map<std::string, std::size_t> occupiedCounts;
  std::unordered_set<std::string> urgentByWorkspace;

  for (const auto& [_, toplevel] : m_toplevels) {
    ++occupiedCounts[toplevel.workspaceKey];
    if (toplevel.urgent) {
      urgentByWorkspace.insert(toplevel.workspaceKey);
    }
  }

  for (auto& workspace : m_workspaces) {
    auto occIt = occupiedCounts.find(workspace.identity.key);
    workspace.occupied = occIt != occupiedCounts.end() && occIt->second > 0;
    workspace.urgent = urgentByWorkspace.contains(workspace.identity.key);
    if (!workspace.monitor.empty()) {
      const auto activeIt = m_activeWorkspaceByMonitor.find(workspace.monitor);
      workspace.active = activeIt != m_activeWorkspaceByMonitor.end() && activeIt->second == workspace.identity.key;
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

void HyprlandWorkspaceBackend::applyWorkspaceIdChange(
    std::uint32_t oldId, std::uint32_t newId, std::string_view newName
) {
  if (oldId == newId || oldId == 0 || newId == 0) {
    return;
  }
  if (m_ipcSchema == IpcSchema::LegacyId && newId > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    refreshSnapshot();
    return;
  }
  const std::string oldKey = std::to_string(oldId);
  const std::string newKey = std::to_string(newId);
  if (findWorkspaceByKey(newKey) != nullptr) {
    refreshSnapshot();
    return;
  }

  auto* workspace = findWorkspaceByKey(oldKey);
  if (workspace == nullptr) {
    refreshSnapshot();
    return;
  }

  workspace->identity.key = newKey;
  workspace->identity.selector = newKey;
  workspace->identity.address = newKey;
  workspace->identity.kind = WorkspaceKind::Numbered;
  workspace->identity.number = newId;
  if (m_ipcSchema == IpcSchema::LegacyId) {
    workspace->identity.legacyId = static_cast<int>(newId);
  }
  if (!newName.empty()) {
    workspace->name = std::string(newName);
  } else if (workspace->name.empty() || workspace->name == std::to_string(oldId)) {
    workspace->name = std::to_string(newId);
  }

  for (auto& [monitor, activeKey] : m_activeWorkspaceByMonitor) {
    if (activeKey == oldKey) {
      activeKey = newKey;
    }
  }
  for (auto& [address, toplevel] : m_toplevels) {
    (void)address;
    if (toplevel.workspaceKey == oldKey) {
      toplevel.workspaceKey = newKey;
    }
  }

  recomputeWorkspaceFlags();
  notifyChanged();
}

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
    const auto identity = parseEventWorkspaceIdentity(args[1]);
    if (!identity.has_value()) {
      refreshSnapshot();
      return;
    }
    handleFocusedMonitor(args[0], identity->key);
    return;
  }

  if (event == "workspacev2") {
    const auto args = parseEventArgs(data, 2);
    const auto identity = parseEventWorkspaceIdentity(args[0], args[1]);
    if (!identity.has_value()) {
      refreshSnapshot();
      return;
    }
    handleWorkspaceActivated(identity->key);
    return;
  }

  if (event == "createworkspacev2") {
    const auto args = parseEventArgs(data, 2);
    const auto identity = parseEventWorkspaceIdentity(args[0], args[1]);
    const std::string name(args[1]);
    if (!identity.has_value() || name.empty()) {
      refreshSnapshot();
      return;
    }
    auto* workspace = findWorkspaceByKey(identity->key);
    if (workspace == nullptr) {
      WorkspaceState state;
      state.identity = *identity;
      state.name = name;
      state.ordinal = m_nextOrdinal++;
      m_workspaces.push_back(std::move(state));
    } else {
      workspace->name = name;
    }
    (void)refreshWorkspaces();
    recomputeWorkspaceFlags();
    notifyChanged();
    return;
  }

  if (event == "destroyworkspacev2") {
    const auto args = parseEventArgs(data, 2);
    const auto identity = parseEventWorkspaceIdentity(args[0], args[1]);
    if (!identity.has_value()) {
      refreshSnapshot();
      return;
    }
    std::erase_if(m_workspaces, [&](const WorkspaceState& ws) { return ws.identity.key == identity->key; });

    for (auto it = m_toplevels.begin(); it != m_toplevels.end();) {
      if (it->second.workspaceKey == identity->key) {
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
    if (m_ipcSchema != IpcSchema::LegacyId) {
      // Stable-identity Hyprland sends a raw address here, which is ambiguous when
      // numbered and named workspaces share the same address.
      refreshSnapshot();
      return;
    }

    const auto args = parseEventArgs(data, 2);
    const auto id = parseInt(args[0]);
    const std::string newName(args[1]);
    if (!id.has_value() || newName.empty()) {
      return;
    }
    auto* workspace = findWorkspaceByKey(std::to_string(*id));
    if (workspace == nullptr) {
      (void)refreshWorkspaces();
      recomputeWorkspaceFlags();
      notifyChanged();
      return;
    }
    workspace->name = newName;

    recomputeWorkspaceFlags();
    notifyChanged();
    return;
  }

  if (event == "changeworkspaceid") {
    // FIXME: confirm payload once Hyprland lands socket2 for change_id
    // (https://github.com/hyprwm/Hyprland/discussions/15527); then remove the
    // ext-workspace reconcile workaround in WaylandWorkspaces::setChangeCallback.
    // Expected: OLDID,NEWID or OLDID,NEWID,NAME.
    const auto args = parseEventArgs(data, 3);
    const auto oldId = parseUnsigned(args[0]);
    const auto newId = parseUnsigned(args[1]);
    if (!oldId.has_value() || !newId.has_value()) {
      refreshSnapshot();
      return;
    }
    applyWorkspaceIdChange(*oldId, *newId, args[2]);
    return;
  }

  if (event == "moveworkspacev2") {
    const auto args = parseEventArgs(data, 3);
    const auto identity = parseEventWorkspaceIdentity(args[0], args[1]);
    const std::string monitor(args[2]);
    if (!identity.has_value()) {
      refreshSnapshot();
      return;
    }
    auto* workspace = findWorkspaceByKey(identity->key);
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
    if (m_ipcSchema == IpcSchema::StableIdentity) {
      refreshClients();
      recomputeWorkspaceFlags();
      notifyChanged();
      return;
    }
    const auto workspace = findWorkspaceByName(workspaceName);
    if (workspace == nullptr) {
      refreshClients();
      recomputeWorkspaceFlags();
      notifyChanged();
      return;
    }
    moveToplevel(*address, workspace->identity.key);
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
    const auto identity = parseEventWorkspaceIdentity(args[1], args[2]);
    if (!address.has_value() || !identity.has_value()) {
      refreshClients();
      recomputeWorkspaceFlags();
      notifyChanged();
      return;
    }
    moveToplevel(*address, identity->key);
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
      m_toplevels.emplace(
          *address,
          ToplevelState{
              .workspaceKey = m_ipcSchema == IpcSchema::LegacyId ? "-1" : std::string{},
              .appId = {},
              .title = {},
              .urgent = true,
          }
      );
      refreshClients();
    } else {
      it->second.urgent = true;
    }
    recomputeWorkspaceFlags();
    notifyChanged();
  }
}

void HyprlandWorkspaceBackend::handleFocusedMonitor(std::string_view monitorName, std::string_view workspaceKey) {
  if (monitorName.empty() || workspaceKey.empty()) {
    return;
  }
  m_activeWorkspaceByMonitor[std::string(monitorName)] = workspaceKey;
  clearUrgentForWorkspace(workspaceKey);
  recomputeWorkspaceFlags();
  notifyChanged();
}

void HyprlandWorkspaceBackend::handleWorkspaceActivated(std::string_view workspaceKey) {
  refreshMonitors();
  clearUrgentForWorkspace(workspaceKey);
  recomputeWorkspaceFlags();
  notifyChanged();
}

void HyprlandWorkspaceBackend::clearUrgentForWorkspace(std::string_view workspaceKey) {
  for (auto& [_, toplevel] : m_toplevels) {
    if (toplevel.workspaceKey == workspaceKey) {
      toplevel.urgent = false;
    }
  }
}

void HyprlandWorkspaceBackend::moveToplevel(std::uint64_t address, std::string_view workspaceKey) {
  auto& toplevel = m_toplevels[address];
  toplevel.workspaceKey = workspaceKey;
}

HyprlandWorkspaceBackend::WorkspaceState* HyprlandWorkspaceBackend::findWorkspaceByKey(std::string_view key) {
  for (auto& workspace : m_workspaces) {
    if (workspace.identity.key == key) {
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

std::optional<HyprlandWorkspaceBackend::IpcSchema>
HyprlandWorkspaceBackend::detectIpcSchema(const nlohmann::json& workspaces) {
  if (!workspaces.is_array() || workspaces.empty()) {
    return std::nullopt;
  }

  std::optional<IpcSchema> schema;
  for (const auto& workspace : workspaces) {
    if (!workspace.is_object()) {
      return std::nullopt;
    }

    const auto idIt = workspace.find("id");
    const auto addressIt = workspace.find("address");
    const auto typeIt = workspace.find("type");
    const bool legacy = idIt != workspace.end()
        && idIt->is_number_integer()
        && addressIt == workspace.end()
        && typeIt == workspace.end();
    const bool stable = idIt == workspace.end()
        && addressIt != workspace.end()
        && addressIt->is_string()
        && typeIt != workspace.end()
        && typeIt->is_string();
    if (legacy == stable) {
      return std::nullopt;
    }

    const IpcSchema itemSchema = legacy ? IpcSchema::LegacyId : IpcSchema::StableIdentity;
    if (schema.has_value() && *schema != itemSchema) {
      return std::nullopt;
    }
    if (!parseJsonWorkspaceIdentity(workspace, itemSchema).has_value()) {
      return std::nullopt;
    }
    schema = itemSchema;
  }
  return schema;
}

std::optional<HyprlandWorkspaceBackend::WorkspaceIdentity>
HyprlandWorkspaceBackend::parseJsonWorkspaceIdentity(const nlohmann::json& json, IpcSchema schema) {
  if (!json.is_object()) {
    return std::nullopt;
  }

  WorkspaceIdentity identity;
  if (schema == IpcSchema::LegacyId) {
    const auto idIt = json.find("id");
    if (idIt == json.end() || !idIt->is_number_integer() || json.contains("address") || json.contains("type")) {
      return std::nullopt;
    }
    const int id = idIt->get<int>();
    identity.key = std::to_string(id);
    identity.legacyId = id;
    if (id >= 0) {
      identity.selector = identity.key;
      identity.address = identity.key;
      identity.kind = WorkspaceKind::Numbered;
      if (id > 0) {
        identity.number = static_cast<std::uint32_t>(id);
      }
      return identity;
    }

    if (const auto nameIt = json.find("name"); nameIt != json.end() && nameIt->is_string()) {
      identity.address = nameIt->get<std::string>();
    }
    if (identity.address == "special" || identity.address.starts_with("special:")) {
      identity.kind = WorkspaceKind::Special;
      identity.selector = identity.address;
    } else {
      identity.kind = WorkspaceKind::Named;
      identity.selector = identity.address.empty() ? std::string{} : "name:" + identity.address;
    }
    return identity;
  }

  if (schema != IpcSchema::StableIdentity) {
    return std::nullopt;
  }

  const auto addressIt = json.find("address");
  const auto typeIt = json.find("type");
  if (json.contains("id")
      || addressIt == json.end()
      || !addressIt->is_string()
      || typeIt == json.end()
      || !typeIt->is_string()) {
    return std::nullopt;
  }
  identity.address = addressIt->get<std::string>();
  const std::string type = typeIt->get<std::string>();
  if (identity.address.empty()) {
    return std::nullopt;
  }

  if (type == "numbered") {
    identity.number = parseUnsigned(identity.address);
    if (!identity.number.has_value()) {
      return std::nullopt;
    }
    identity.kind = WorkspaceKind::Numbered;
    identity.key = identity.address;
    identity.selector = identity.address;
  } else if (type == "named") {
    identity.kind = WorkspaceKind::Named;
    identity.key = "name:" + identity.address;
    identity.selector = identity.key;
  } else if (type == "special") {
    identity.kind = WorkspaceKind::Special;
    identity.key = identity.address;
    identity.selector = identity.address;
  } else {
    return std::nullopt;
  }
  return identity;
}

std::optional<HyprlandWorkspaceBackend::WorkspaceIdentity>
HyprlandWorkspaceBackend::parseEventWorkspaceIdentity(std::string_view selector, std::string_view displayName) const {
  if (selector.empty() || m_ipcSchema == IpcSchema::Unknown) {
    return std::nullopt;
  }

  if (m_ipcSchema == IpcSchema::LegacyId) {
    const auto id = parseInt(selector);
    if (!id.has_value()) {
      return std::nullopt;
    }
    const std::string key = std::to_string(*id);
    for (const auto& workspace : m_workspaces) {
      if (workspace.identity.key == key) {
        return workspace.identity;
      }
    }

    WorkspaceIdentity identity;
    identity.key = key;
    identity.legacyId = id;
    if (*id >= 0) {
      identity.selector = key;
      identity.address = key;
      identity.kind = WorkspaceKind::Numbered;
      if (*id > 0) {
        identity.number = static_cast<std::uint32_t>(*id);
      }
    } else {
      identity.address = displayName;
      if (displayName == "special" || displayName.starts_with("special:")) {
        identity.kind = WorkspaceKind::Special;
        identity.selector = displayName;
      } else {
        identity.kind = WorkspaceKind::Named;
        identity.selector = displayName.empty() ? std::string{} : "name:" + std::string(displayName);
      }
    }
    return identity;
  }

  WorkspaceIdentity identity;
  if (selector.starts_with("name:")) {
    identity.address = selector.substr(5);
    if (identity.address.empty()) {
      return std::nullopt;
    }
    identity.kind = WorkspaceKind::Named;
    identity.key = selector;
    identity.selector = selector;
    return identity;
  }
  if (selector == "special" || selector.starts_with("special:")) {
    identity.address = selector;
    identity.kind = WorkspaceKind::Special;
    identity.key = selector;
    identity.selector = selector;
    return identity;
  }
  identity.number = parseUnsigned(selector);
  if (!identity.number.has_value()) {
    return std::nullopt;
  }
  identity.address = selector;
  identity.kind = WorkspaceKind::Numbered;
  identity.key = selector;
  identity.selector = selector;
  return identity;
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

std::optional<std::uint32_t> HyprlandWorkspaceBackend::parseUnsigned(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint32_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc{} || ptr != end || parsed == 0) {
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
    args.emplace_back();
  }
  return args;
}

bool HyprlandWorkspaceBackend::isSpecial(const WorkspaceState& state) {
  return state.identity.kind == WorkspaceKind::Special;
}

// Stable-identity Hyprland traverses named workspaces newest-first, followed by numbered workspaces.
bool HyprlandWorkspaceBackend::workspaceOrderLess(const WorkspaceState* a, const WorkspaceState* b) {
  if (a->identity.legacyId.has_value() && b->identity.legacyId.has_value()) {
    return *a->identity.legacyId < *b->identity.legacyId;
  }

  if (a->identity.kind != b->identity.kind) {
    const auto rank = [](WorkspaceKind kind) {
      switch (kind) {
      case WorkspaceKind::Named:
        return 0;
      case WorkspaceKind::Numbered:
        return 1;
      case WorkspaceKind::Special:
        return 2;
      }
      return 3;
    };
    return rank(a->identity.kind) < rank(b->identity.kind);
  }
  if (a->identity.kind == WorkspaceKind::Named) {
    return a->ordinal != b->ordinal ? a->ordinal > b->ordinal : a->identity.key < b->identity.key;
  }
  if (a->identity.number != b->identity.number) {
    return a->identity.number < b->identity.number;
  }
  return a->identity.key < b->identity.key;
}

Workspace HyprlandWorkspaceBackend::toWorkspace(const WorkspaceState& state) {
  const std::uint32_t coord =
      state.identity.number.has_value() ? *state.identity.number - 1 : static_cast<std::uint32_t>(state.ordinal);
  return Workspace{
      .id = state.identity.key,
      .name = state.name,
      .coordinates = {coord},
      .active = state.active,
      .urgent = state.urgent,
      .occupied = state.occupied,
  };
}

std::string HyprlandWorkspaceBackend::assignmentKeyFor(std::string_view workspaceKey) const {
  for (const auto& ws : m_workspaces) {
    if (ws.identity.key == workspaceKey && ws.identity.legacyId.value_or(0) < 0 && !ws.name.empty()) {
      return ws.name;
    }
  }
  return std::string(workspaceKey);
}
