#pragma once

#include "compositors/workspace_backend.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Tracks user-requested "alert" markers for workspaces. An alert is stored as
// the token the user supplied and matched against a workspace's user-
// addressable identifiers -- its id, its name, or its visible index -- because
// different compositors populate different ones (niri only sets the visible
// index; Sway uses the name as the id; tag compositors use the numeric id).
// Alerts are surfaced by reusing the workspace urgent flag, so the feature
// needs no new per-backend identifier.
//
// Note: none of these identifiers is guaranteed unique across outputs on
// compositors that number workspaces per monitor (niri index, mango/dwl tags),
// so an alert there applies to the matching workspace on every output.
class WorkspaceAlertService {
public:
  [[nodiscard]] bool add(std::string_view token);
  [[nodiscard]] bool clear(std::string_view token);
  void clearAll();

  [[nodiscard]] bool contains(std::string_view token) const;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::vector<std::string> tokens() const;

  void applyOverlay(std::vector<Workspace>& workspaces) const;
  [[nodiscard]] std::size_t clearActive(const std::vector<Workspace>& workspaces);

  [[nodiscard]] static bool isKnownWorkspaceToken(std::string_view token, const std::vector<Workspace>& workspaces);
  [[nodiscard]] static std::optional<std::string>
  workspaceTokenForWindow(std::string_view windowId, const std::vector<WorkspaceWindowAssignment>& assignments);

private:
  [[nodiscard]] bool isAlerted(const Workspace& workspace) const;

  std::set<std::string, std::less<>> m_alerts;
};
