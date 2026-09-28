#pragma once

#include "compositors/workspace_backend.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Decides which windows sit on the workspace the user is looking at. Kept as pure
// functions over plain data so the per-backend matching rules stay testable.
namespace switcher_membership {

  struct OutputMembership {
    std::vector<Workspace> workspaces;
    std::vector<std::string> overlayKeys;
    std::string focusedKey;
    std::vector<WorkspaceWindowAssignment> assignments;
  };

  [[nodiscard]] inline bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
    if (assignmentKey.empty()) {
      return false;
    }
    if (!workspace.id.empty() && assignmentKey == workspace.id) {
      return true;
    }
    if (!workspace.name.empty() && assignmentKey == workspace.name) {
      return true;
    }
    if (workspace.index > 0 && assignmentKey == std::to_string(workspace.index)) {
      return true;
    }
    // Sway: id/name are "1: web" while assignments use "1". Skip when id != name
    // so Hyprland named workspaces do not match numbered workspace "1".
    if (workspace.id.empty() || workspace.id != workspace.name) {
      return false;
    }
    const auto leadingNumericAssignmentKey = [](std::string_view label) -> std::optional<std::string_view> {
      std::size_t digits = 0;
      while (digits < label.size() && std::isdigit(static_cast<unsigned char>(label[digits])) != 0) {
        ++digits;
      }
      if (digits == 0) {
        return std::nullopt;
      }
      if (digits < label.size() && label[digits] != ':' && label[digits] != ' ') {
        return std::nullopt;
      }
      return label.substr(0, digits);
    };
    if (const auto prefix = leadingNumericAssignmentKey(workspace.name);
        prefix.has_value() && assignmentKey == *prefix) {
      return true;
    }
    return false;
  }

  [[nodiscard]] inline bool
  assignmentOnActiveWorkspace(std::string_view workspaceKey, const std::vector<Workspace>& workspaces) {
    if (workspaceKey.empty()) {
      return false;
    }
    for (const auto& workspace : workspaces) {
      if (workspace.active && workspaceKeyMatchesAssignment(workspaceKey, workspace)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] inline bool
  assignmentOnVisibleWorkspace(const OutputMembership& membership, std::string_view assignmentKey) {
    if (assignmentKey.empty()) {
      return false;
    }
    if (!membership.overlayKeys.empty()) {
      return std::ranges::any_of(membership.overlayKeys, [assignmentKey](const std::string& key) {
        return std::string_view(key) == assignmentKey;
      });
    }
    if (membership.workspaces.empty()) {
      // No workspace model for this output: filtering would hide every window.
      return true;
    }
    if (assignmentOnActiveWorkspace(assignmentKey, membership.workspaces)) {
      return true;
    }
    const bool hasActive =
        std::ranges::any_of(membership.workspaces, [](const Workspace& workspace) { return workspace.active; });
    return !hasActive && !membership.focusedKey.empty() && std::string_view(membership.focusedKey) == assignmentKey;
  }

  // Without assignments from any output the switcher cannot tell which workspace a window
  // is on, so the filter must stay off.
  [[nodiscard]] inline bool hasWorkspaceMembershipData(const std::vector<OutputMembership>& memberships) {
    return std::ranges::any_of(memberships, [](const OutputMembership& membership) {
      return !membership.assignments.empty();
    });
  }

  // Callers check hasWorkspaceMembershipData() first; order is preserved for their sorting.
  [[nodiscard]] inline std::vector<WorkspaceWindowAssignment>
  assignmentsOnVisibleWorkspaces(const std::vector<OutputMembership>& memberships) {
    std::vector<WorkspaceWindowAssignment> matched;
    for (const auto& membership : memberships) {
      for (const auto& assignment : membership.assignments) {
        if (assignment.windowId.empty()) {
          continue;
        }
        if (assignmentOnVisibleWorkspace(membership, assignment.workspaceKey)) {
          matched.push_back(assignment);
        }
      }
    }
    return matched;
  }

} // namespace switcher_membership
