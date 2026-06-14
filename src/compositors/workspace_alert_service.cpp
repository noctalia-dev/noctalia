#include "compositors/workspace_alert_service.h"

#include <algorithm>

bool WorkspaceAlertService::add(std::string_view workspaceKey) {
  if (workspaceKey.empty()) {
    return false;
  }
  return m_alerts.emplace(std::string{workspaceKey}).second;
}

bool WorkspaceAlertService::clear(std::string_view workspaceKey) {
  if (workspaceKey.empty()) {
    return false;
  }
  const auto it = m_alerts.find(workspaceKey);
  if (it == m_alerts.end()) {
    return false;
  }
  m_alerts.erase(it);
  return true;
}

void WorkspaceAlertService::clearAll() { m_alerts.clear(); }

bool WorkspaceAlertService::contains(std::string_view workspaceKey) const {
  return !workspaceKey.empty() && m_alerts.contains(workspaceKey);
}

bool WorkspaceAlertService::empty() const noexcept { return m_alerts.empty(); }

std::vector<std::string> WorkspaceAlertService::keys() const {
  return {m_alerts.begin(), m_alerts.end()};
}

void WorkspaceAlertService::applyOverlay(std::vector<Workspace>& workspaces) const {
  if (m_alerts.empty()) {
    return;
  }
  for (auto& workspace : workspaces) {
    if (!workspace.active && contains(workspace.key)) {
      workspace.urgent = true;
    }
  }
}

std::size_t WorkspaceAlertService::clearActive(const std::vector<Workspace>& workspaces) {
  std::size_t cleared = 0;
  for (const auto& workspace : workspaces) {
    if (!workspace.active || workspace.key.empty()) {
      continue;
    }
    if (clear(workspace.key)) {
      ++cleared;
    }
  }
  return cleared;
}

bool WorkspaceAlertService::isKnownWorkspaceKey(
    std::string_view workspaceKey, const std::vector<Workspace>& workspaces
) {
  if (workspaceKey.empty()) {
    return false;
  }
  return std::any_of(workspaces.begin(), workspaces.end(), [&](const Workspace& workspace) {
    return workspace.key == workspaceKey;
  });
}

std::optional<std::string> WorkspaceAlertService::workspaceKeyForWindow(
    std::string_view windowId, const std::vector<WorkspaceWindowAssignment>& assignments
) {
  if (windowId.empty()) {
    return std::nullopt;
  }
  for (const auto& assignment : assignments) {
    if (assignment.windowId == windowId && !assignment.workspaceKey.empty()) {
      return assignment.workspaceKey;
    }
  }
  return std::nullopt;
}
