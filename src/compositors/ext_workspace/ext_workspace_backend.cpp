#include "compositors/ext_workspace/ext_workspace_backend.h"

#include "core/log.h"
#include "ext-workspace-v1-client-protocol.h"

#include <algorithm>

namespace {

  constexpr Logger kLog("workspace_ext");

  void groupCapabilities(void* /*data*/, ext_workspace_group_handle_v1* /*group*/, uint32_t /*caps*/) {}

  void groupOutputEnter(void* data, ext_workspace_group_handle_v1* group, wl_output* output) {
    static_cast<ExtWorkspaceBackend*>(data)->onGroupOutputEnter(group, output);
  }

  void groupOutputLeave(void* data, ext_workspace_group_handle_v1* group, wl_output* output) {
    static_cast<ExtWorkspaceBackend*>(data)->onGroupOutputLeave(group, output);
  }

  void groupWorkspaceEnter(void* data, ext_workspace_group_handle_v1* group, ext_workspace_handle_v1* workspace) {
    static_cast<ExtWorkspaceBackend*>(data)->onGroupWorkspaceEnter(group, workspace);
  }

  void groupWorkspaceLeave(void* data, ext_workspace_group_handle_v1* group, ext_workspace_handle_v1* workspace) {
    static_cast<ExtWorkspaceBackend*>(data)->onGroupWorkspaceLeave(group, workspace);
  }

  void groupRemoved(void* data, ext_workspace_group_handle_v1* group) {
    static_cast<ExtWorkspaceBackend*>(data)->onGroupRemoved(group);
  }

  const ext_workspace_group_handle_v1_listener kGroupListener = {
      .capabilities = groupCapabilities,
      .output_enter = groupOutputEnter,
      .output_leave = groupOutputLeave,
      .workspace_enter = groupWorkspaceEnter,
      .workspace_leave = groupWorkspaceLeave,
      .removed = groupRemoved,
  };

  void workspaceId(void* data, ext_workspace_handle_v1* workspace, const char* id) {
    static_cast<ExtWorkspaceBackend*>(data)->onWorkspaceIdChanged(workspace, id);
  }

  void workspaceName(void* data, ext_workspace_handle_v1* workspace, const char* name) {
    static_cast<ExtWorkspaceBackend*>(data)->onWorkspaceNameChanged(workspace, name);
  }

  void workspaceCoordinates(void* data, ext_workspace_handle_v1* workspace, wl_array* coords) {
    static_cast<ExtWorkspaceBackend*>(data)->onWorkspaceCoordinatesChanged(workspace, coords);
  }

  void workspaceState(void* data, ext_workspace_handle_v1* workspace, uint32_t state) {
    static_cast<ExtWorkspaceBackend*>(data)->onWorkspaceStateChanged(workspace, state);
  }

  void workspaceCapabilities(void* /*data*/, ext_workspace_handle_v1* /*workspace*/, uint32_t /*caps*/) {}

  void workspaceRemoved(void* data, ext_workspace_handle_v1* workspace) {
    static_cast<ExtWorkspaceBackend*>(data)->onWorkspaceRemoved(workspace);
  }

  const ext_workspace_handle_v1_listener kWorkspaceListener = {
      .id = workspaceId,
      .name = workspaceName,
      .coordinates = workspaceCoordinates,
      .state = workspaceState,
      .capabilities = workspaceCapabilities,
      .removed = workspaceRemoved,
  };

  void managerWorkspaceGroup(void* data, ext_workspace_manager_v1* /*manager*/, ext_workspace_group_handle_v1* group) {
    static_cast<ExtWorkspaceBackend*>(data)->onGroupCreated(group);
  }

  void managerWorkspace(void* data, ext_workspace_manager_v1* /*manager*/, ext_workspace_handle_v1* workspace) {
    static_cast<ExtWorkspaceBackend*>(data)->onWorkspaceCreated(workspace);
  }

  void managerDone(void* data, ext_workspace_manager_v1* /*manager*/) {
    static_cast<ExtWorkspaceBackend*>(data)->onManagerDone();
  }

  void managerFinished(void* data, ext_workspace_manager_v1* /*manager*/) {
    static_cast<ExtWorkspaceBackend*>(data)->onManagerFinished();
  }

  const ext_workspace_manager_v1_listener kManagerListener = {
      .workspace_group = managerWorkspaceGroup,
      .workspace = managerWorkspace,
      .done = managerDone,
      .finished = managerFinished,
  };

  std::vector<std::uint32_t> normalizeCoordinates(const std::vector<std::uint32_t>& coords) {
    auto normalized = coords;
    while (!normalized.empty() && normalized.back() == 0) {
      normalized.pop_back();
    }
    return normalized;
  }

} // namespace

void ExtWorkspaceBackend::bindExtWorkspace(ext_workspace_manager_v1* manager) {
  m_manager = manager;
  ext_workspace_manager_v1_add_listener(m_manager, &kManagerListener, this);
}

void ExtWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void ExtWorkspaceBackend::activate(const std::string& id) {
  if (m_manager == nullptr) {
    return;
  }

  for (const auto& [handle, ws] : m_workspaces) {
    if (ws.id != id) {
      continue;
    }
    ext_workspace_handle_v1_activate(handle);
    ext_workspace_manager_v1_commit(m_manager);
    kLog.debug("activating \"{}\"", ws.name);
    return;
  }
}

void ExtWorkspaceBackend::activateForOutput(wl_output* output, const std::string& id) {
  if (m_manager == nullptr || output == nullptr) {
    return;
  }

  for (const auto& group : m_groups) {
    const bool hasOutput = std::find(group.outputs.begin(), group.outputs.end(), output) != group.outputs.end();
    if (!hasOutput) {
      continue;
    }

    for (auto* handle : group.workspaces) {
      const auto it = m_workspaces.find(handle);
      if (it == m_workspaces.end() || it->second.id != id) {
        continue;
      }

      ext_workspace_handle_v1_activate(handle);
      ext_workspace_manager_v1_commit(m_manager);
      kLog.debug("activating \"{}\"", it->second.name);
      return;
    }
  }

  activate(id);
}

void ExtWorkspaceBackend::activateForOutput(wl_output* output, const Workspace& workspace) {
  if (m_manager == nullptr || output == nullptr) {
    return;
  }

  auto matchesExact = [&](const Workspace& candidate) {
    return candidate.id == workspace.id && candidate.name == workspace.name &&
           normalizeCoordinates(candidate.coordinates) == normalizeCoordinates(workspace.coordinates);
  };
  auto matchesId = [&](const Workspace& candidate) { return !workspace.id.empty() && candidate.id == workspace.id; };
  auto matchesCoordinatesPrimary = [&](const Workspace& candidate) {
    const auto wanted = normalizeCoordinates(workspace.coordinates);
    const auto have = normalizeCoordinates(candidate.coordinates);
    if (wanted.empty() || have.empty()) {
      return false;
    }
    return wanted[0] == have[0];
  };

  for (const auto& group : m_groups) {
    const bool hasOutput = std::find(group.outputs.begin(), group.outputs.end(), output) != group.outputs.end();
    if (!hasOutput) {
      continue;
    }

    for (auto* handle : group.workspaces) {
      const auto it = m_workspaces.find(handle);
      if (it == m_workspaces.end() || !matchesExact(it->second)) {
        continue;
      }
      ext_workspace_handle_v1_activate(handle);
      ext_workspace_manager_v1_commit(m_manager);
      kLog.debug("activating \"{}\"", it->second.name);
      return;
    }

    for (auto* handle : group.workspaces) {
      const auto it = m_workspaces.find(handle);
      if (it == m_workspaces.end() || !matchesId(it->second)) {
        continue;
      }
      ext_workspace_handle_v1_activate(handle);
      ext_workspace_manager_v1_commit(m_manager);
      kLog.debug("activating \"{}\"", it->second.name);
      return;
    }

    for (auto* handle : group.workspaces) {
      const auto it = m_workspaces.find(handle);
      if (it == m_workspaces.end() || !matchesCoordinatesPrimary(it->second)) {
        continue;
      }
      ext_workspace_handle_v1_activate(handle);
      ext_workspace_manager_v1_commit(m_manager);
      kLog.debug("activating \"{}\" (coordinate fallback)", it->second.name);
      return;
    }
  }

  if (!workspace.id.empty()) {
    activate(workspace.id);
  }
}

void ExtWorkspaceBackend::cleanup() {
  for (auto& [workspace, _] : m_workspaces) {
    if (workspace != nullptr) {
      ext_workspace_handle_v1_destroy(workspace);
    }
  }
  m_workspaces.clear();

  for (auto& group : m_groups) {
    if (group.handle != nullptr) {
      ext_workspace_group_handle_v1_destroy(group.handle);
    }
  }
  m_groups.clear();

  if (m_manager != nullptr) {
    ext_workspace_manager_v1_stop(m_manager);
    ext_workspace_manager_v1_destroy(m_manager);
    m_manager = nullptr;
  }
}

std::vector<Workspace> ExtWorkspaceBackend::all() const {
  std::vector<Workspace> result;
  std::vector<ext_workspace_handle_v1*> seen;

  for (const auto& group : m_groups) {
    for (auto* handle : group.workspaces) {
      if (std::find(seen.begin(), seen.end(), handle) != seen.end()) {
        continue;
      }
      const auto it = m_workspaces.find(handle);
      if (it != m_workspaces.end() && !it->second.name.empty()) {
        result.push_back(it->second);
        seen.push_back(handle);
      }
    }
  }

  for (const auto& [handle, ws] : m_workspaces) {
    if (ws.name.empty() || std::find(seen.begin(), seen.end(), handle) != seen.end()) {
      continue;
    }
    result.push_back(ws);
  }

  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.coordinates < b.coordinates; });
  return result;
}

std::vector<Workspace> ExtWorkspaceBackend::forOutput(wl_output* output) const {
  std::vector<ext_workspace_handle_v1*> handles;
  for (const auto& group : m_groups) {
    const bool hasOutput = std::find(group.outputs.begin(), group.outputs.end(), output) != group.outputs.end();
    if (hasOutput) {
      handles.insert(handles.end(), group.workspaces.begin(), group.workspaces.end());
    }
  }

  std::vector<Workspace> result;
  for (auto* handle : handles) {
    const auto it = m_workspaces.find(handle);
    if (it != m_workspaces.end() && !it->second.name.empty()) {
      result.push_back(it->second);
    }
  }

  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.coordinates < b.coordinates; });
  return result;
}

void ExtWorkspaceBackend::onGroupCreated(ext_workspace_group_handle_v1* group) {
  if (group == nullptr) {
    return;
  }
  m_groups.push_back(WorkspaceGroup{.handle = group, .outputs = {}, .workspaces = {}});
  ext_workspace_group_handle_v1_add_listener(group, &kGroupListener, this);
}

void ExtWorkspaceBackend::onGroupRemoved(ext_workspace_group_handle_v1* group) {
  std::erase_if(m_groups, [group](const auto& g) { return g.handle == group; });
  if (group != nullptr) {
    ext_workspace_group_handle_v1_destroy(group);
  }
}

void ExtWorkspaceBackend::onGroupOutputEnter(ext_workspace_group_handle_v1* group, wl_output* output) {
  for (auto& g : m_groups) {
    if (g.handle == group) {
      g.outputs.push_back(output);
      return;
    }
  }
}

void ExtWorkspaceBackend::onGroupOutputLeave(ext_workspace_group_handle_v1* group, wl_output* output) {
  for (auto& g : m_groups) {
    if (g.handle == group) {
      std::erase(g.outputs, output);
      return;
    }
  }
}

void ExtWorkspaceBackend::onGroupWorkspaceEnter(
    ext_workspace_group_handle_v1* group, ext_workspace_handle_v1* workspace
) {
  for (auto& g : m_groups) {
    if (g.handle == group) {
      g.workspaces.push_back(workspace);
      return;
    }
  }
}

void ExtWorkspaceBackend::onGroupWorkspaceLeave(
    ext_workspace_group_handle_v1* group, ext_workspace_handle_v1* workspace
) {
  for (auto& g : m_groups) {
    if (g.handle == group) {
      std::erase(g.workspaces, workspace);
      return;
    }
  }
}

void ExtWorkspaceBackend::onWorkspaceCreated(ext_workspace_handle_v1* workspace) {
  if (workspace == nullptr) {
    return;
  }
  m_workspaces.emplace(workspace, Workspace{});
  ext_workspace_handle_v1_add_listener(workspace, &kWorkspaceListener, this);
}

void ExtWorkspaceBackend::onWorkspaceIdChanged(ext_workspace_handle_v1* workspace, const char* id) {
  const auto it = m_workspaces.find(workspace);
  if (it != m_workspaces.end()) {
    it->second.id = id != nullptr ? id : "";
  }
}

void ExtWorkspaceBackend::onWorkspaceNameChanged(ext_workspace_handle_v1* workspace, const char* name) {
  const auto it = m_workspaces.find(workspace);
  if (it != m_workspaces.end()) {
    it->second.name = name != nullptr ? name : "";
  }
}

void ExtWorkspaceBackend::onWorkspaceCoordinatesChanged(ext_workspace_handle_v1* workspace, wl_array* coordinates) {
  const auto it = m_workspaces.find(workspace);
  if (it == m_workspaces.end()) {
    return;
  }

  it->second.coordinates.clear();
  if (coordinates != nullptr) {
    const auto* coords = static_cast<std::uint32_t*>(coordinates->data);
    const auto count = coordinates->size / sizeof(std::uint32_t);
    it->second.coordinates.assign(coords, coords + count);
  }
}

void ExtWorkspaceBackend::onWorkspaceStateChanged(ext_workspace_handle_v1* workspace, std::uint32_t state) {
  const auto it = m_workspaces.find(workspace);
  if (it == m_workspaces.end()) {
    return;
  }

  const bool isActive = (state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0;
  const bool isUrgent = (state & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT) != 0;
  if (it->second.active == isActive && it->second.urgent == isUrgent) {
    return;
  }

  it->second.active = isActive;
  it->second.urgent = isUrgent;
  if (isActive) {
    kLog.debug("active: {}", it->second.name.empty() ? "(unnamed)" : it->second.name);
  }
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void ExtWorkspaceBackend::onWorkspaceRemoved(ext_workspace_handle_v1* workspace) {
  m_workspaces.erase(workspace);
  if (workspace != nullptr) {
    ext_workspace_handle_v1_destroy(workspace);
  }
}

void ExtWorkspaceBackend::onManagerDone() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void ExtWorkspaceBackend::onManagerFinished() {
  m_manager = nullptr;
  m_workspaces.clear();
  m_groups.clear();
}
