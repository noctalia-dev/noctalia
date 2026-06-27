#include "compositors/labwc/labwc_workspace_backend.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace {

  [[nodiscard]] std::optional<std::size_t> parseLeadingNumber(const std::string& value) {
    if (value.empty() || !std::isdigit(static_cast<unsigned char>(value.front()))) {
      return std::nullopt;
    }
    std::size_t parsed = 0;
    std::size_t i = 0;
    while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
      parsed = parsed * 10 + static_cast<std::size_t>(value[i] - '0');
      ++i;
    }
    return parsed > 0 ? std::optional<std::size_t>(parsed) : std::nullopt;
  }

} // namespace

void LabwcWorkspaceBackend::setProviders(WorkspacesProvider workspaces, ToplevelsProvider toplevels) {
  m_workspacesProvider = std::move(workspaces);
  m_toplevelsProvider = std::move(toplevels);
}

void LabwcWorkspaceBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void LabwcWorkspaceBackend::cleanup() {
  m_windows.clear();
  m_workspacesProvider = {};
  m_toplevelsProvider = {};
}

std::string LabwcWorkspaceBackend::workspaceKeyFor(const Workspace& workspace, const std::size_t index) {
  if (const auto id = parseLeadingNumber(workspace.id); id.has_value()) {
    return std::to_string(*id);
  }
  if (const auto name = parseLeadingNumber(workspace.name); name.has_value()) {
    return std::to_string(*name);
  }
  if (!workspace.id.empty()) {
    return workspace.id;
  }
  if (!workspace.name.empty()) {
    return workspace.name;
  }
  if (!workspace.coordinates.empty()) {
    return std::to_string(static_cast<std::size_t>(workspace.coordinates.front()) + 1u);
  }
  return std::to_string(index + 1);
}

std::string LabwcWorkspaceBackend::activeWorkspaceKey(const std::vector<Workspace>& workspaces) const {
  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    if (workspaces[i].active) {
      return workspaceKeyFor(workspaces[i], i);
    }
  }
  return workspaces.empty() ? std::string{} : workspaceKeyFor(workspaces.front(), 0);
}

bool LabwcWorkspaceBackend::sync() {
  if (!m_workspacesProvider || !m_toplevelsProvider) {
    return false;
  }

  const auto workspaces = m_workspacesProvider();
  const std::string activeKey = activeWorkspaceKey(workspaces);

  std::unordered_map<std::uintptr_t, TrackedWindow> next;
  m_toplevelsProvider([&](const WlrToplevelSnapshot& toplevel) {
    if (toplevel.handle == nullptr || toplevel.appId.empty()) {
      return;
    }

    const auto handleKey = reinterpret_cast<std::uintptr_t>(toplevel.handle);
    std::string workspaceKey;
    if (!toplevel.minimized) {
      workspaceKey = activeKey;
    } else if (const auto it = m_windows.find(handleKey); it != m_windows.end()) {
      workspaceKey = it->second.workspaceKey;
    }
    if (workspaceKey.empty()) {
      return;
    }

    next.emplace(
        handleKey,
        TrackedWindow{
            .workspaceKey = std::move(workspaceKey),
            .appId = toplevel.appId,
            .title = toplevel.title,
        }
    );
  });

  if (next == m_windows) {
    return false;
  }
  m_windows = std::move(next);
  return true;
}

void LabwcWorkspaceBackend::apply(std::vector<Workspace>& workspaces, const std::string& /*outputName*/) const {
  std::unordered_set<std::string> occupied;
  for (const auto& [_, window] : m_windows) {
    if (!window.workspaceKey.empty()) {
      occupied.insert(window.workspaceKey);
    }
  }

  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    workspaces[i].occupied = occupied.contains(workspaceKeyFor(workspaces[i], i));
  }
}

std::vector<std::string> LabwcWorkspaceBackend::workspaceKeys(const std::string& /*outputName*/) const {
  if (!m_workspacesProvider) {
    return {};
  }
  const auto workspaces = m_workspacesProvider();
  std::vector<std::string> keys;
  keys.reserve(workspaces.size());
  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    keys.push_back(workspaceKeyFor(workspaces[i], i));
  }
  return keys;
}

std::unordered_map<std::string, std::vector<std::string>>
LabwcWorkspaceBackend::appIdsByWorkspace(const std::string& /*outputName*/) const {
  std::unordered_map<std::string, std::vector<std::string>> grouped;
  std::unordered_map<std::string, std::unordered_set<std::string>> seen;
  for (const auto& [_, window] : m_windows) {
    if (window.workspaceKey.empty() || window.appId.empty()) {
      continue;
    }
    auto& workspaceSeen = seen[window.workspaceKey];
    if (workspaceSeen.insert(window.appId).second) {
      grouped[window.workspaceKey].push_back(window.appId);
    }
  }
  return grouped;
}

std::vector<WorkspaceWindow> LabwcWorkspaceBackend::workspaceWindows(const std::string& /*outputName*/) const {
  std::vector<WorkspaceWindow> result;
  result.reserve(m_windows.size());
  for (const auto& [handleKey, window] : m_windows) {
    (void)handleKey;
    if (window.workspaceKey.empty() || window.appId.empty()) {
      continue;
    }
    result.push_back(
        WorkspaceWindow{
            .windowId = std::to_string(handleKey),
            .workspaceKey = window.workspaceKey,
            .appId = window.appId,
            .title = window.title,
            .x = window.x,
            .y = window.y,
            .outputName = {},
        }
    );
  }
  std::ranges::stable_sort(result, {}, &WorkspaceWindow::workspaceKey);
  return result;
}
