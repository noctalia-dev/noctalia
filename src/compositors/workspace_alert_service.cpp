#include "compositors/workspace_alert_service.h"

#include <algorithm>
#include <string>

namespace {

  // A token matches a workspace if it equals any of the workspace's
  // user-addressable identifiers. Backends populate different ones, so all
  // three are considered (empty/zero values never match).
  bool identifierMatches(const Workspace& workspace, std::string_view token) {
    return (!workspace.id.empty() && workspace.id == token)
        || (!workspace.name.empty() && workspace.name == token)
        || (workspace.index > 0 && std::to_string(workspace.index) == token);
  }

} // namespace

bool WorkspaceAlertService::add(std::string_view token) {
  if (token.empty()) {
    return false;
  }
  return m_alerts.emplace(std::string{token}).second;
}

bool WorkspaceAlertService::clear(std::string_view token) {
  if (token.empty()) {
    return false;
  }
  const auto it = m_alerts.find(token);
  if (it == m_alerts.end()) {
    return false;
  }
  m_alerts.erase(it);
  return true;
}

void WorkspaceAlertService::clearAll() { m_alerts.clear(); }

bool WorkspaceAlertService::contains(std::string_view token) const {
  return !token.empty() && m_alerts.contains(token);
}

bool WorkspaceAlertService::empty() const noexcept { return m_alerts.empty(); }

std::vector<std::string> WorkspaceAlertService::tokens() const {
  return {m_alerts.begin(), m_alerts.end()};
}

bool WorkspaceAlertService::isAlerted(const Workspace& workspace) const {
  return (!workspace.id.empty() && m_alerts.contains(workspace.id))
      || (!workspace.name.empty() && m_alerts.contains(workspace.name))
      || (workspace.index > 0 && m_alerts.contains(std::to_string(workspace.index)));
}

void WorkspaceAlertService::applyOverlay(std::vector<Workspace>& workspaces) const {
  if (m_alerts.empty()) {
    return;
  }
  for (auto& workspace : workspaces) {
    if (!workspace.active && isAlerted(workspace)) {
      workspace.urgent = true;
    }
  }
}

std::size_t WorkspaceAlertService::clearActive(const std::vector<Workspace>& workspaces) {
  if (m_alerts.empty()) {
    return 0;
  }
  std::size_t cleared = 0;
  for (const auto& workspace : workspaces) {
    if (!workspace.active) {
      continue;
    }
    // Remove whichever token form the user supplied for this workspace.
    if (!workspace.id.empty() && clear(workspace.id)) {
      ++cleared;
    }
    if (!workspace.name.empty() && clear(workspace.name)) {
      ++cleared;
    }
    if (workspace.index > 0 && clear(std::to_string(workspace.index))) {
      ++cleared;
    }
  }
  return cleared;
}

bool WorkspaceAlertService::isKnownWorkspaceToken(
    std::string_view token, const std::vector<Workspace>& workspaces
) {
  if (token.empty()) {
    return false;
  }
  return std::any_of(workspaces.begin(), workspaces.end(), [&](const Workspace& workspace) {
    return identifierMatches(workspace, token);
  });
}

std::optional<std::string> WorkspaceAlertService::workspaceTokenForWindow(
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
