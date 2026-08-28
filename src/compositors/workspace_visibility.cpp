#include "compositors/workspace_visibility.h"

#include <string>
#include <unordered_map>

namespace compositors {

  bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
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
    return false;
  }

  bool activeWorkspaceHasVisibleWindows(
      const std::vector<Workspace>& workspaces, const std::vector<WorkspaceWindowAssignment>& assignments
  ) {
    const Workspace* active = nullptr;
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        active = &workspace;
        break;
      }
    }
    if (active == nullptr) {
      return false;
    }

    bool hasOnActive = false;
    for (const auto& assignment : assignments) {
      if (!workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
        continue;
      }
      hasOnActive = true;
      if (!assignment.minimized) {
        return true;
      }
    }
    if (hasOnActive) {
      return false;
    }
    if (!assignments.empty()) {
      return false;
    }
    return active->occupied;
  }

  void enrichAssignmentsWithMinimizedState(
      std::vector<WorkspaceWindowAssignment>& assignments, const std::vector<WlrToplevelSnapshot>& minimizedToplevels
  ) {
    std::unordered_map<std::string, bool> minimizedKeys;
    for (const auto& toplevel : minimizedToplevels) {
      if (toplevel.handle != nullptr) {
        minimizedKeys.emplace(std::to_string(reinterpret_cast<std::uintptr_t>(toplevel.handle)), true);
      }
      if (!toplevel.appId.empty() || !toplevel.title.empty()) {
        minimizedKeys.emplace(toplevel.appId + ":" + toplevel.title, true);
      }
    }
    for (auto& assignment : assignments) {
      if (assignment.minimized) {
        continue;
      }
      if (minimizedKeys.contains(assignment.windowId)
          || minimizedKeys.contains(assignment.appId + ":" + assignment.title)) {
        assignment.minimized = true;
      }
    }
  }

} // namespace compositors
