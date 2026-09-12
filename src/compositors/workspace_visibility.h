#pragma once

#include "compositors/workspace_backend.h"
#include "wayland/wayland_toplevels.h"

#include <string_view>
#include <vector>

namespace compositors {

  [[nodiscard]] bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace);

  // True when the active workspace has at least one non-minimized window assignment.
  [[nodiscard]] bool activeWorkspaceHasVisibleWindows(
      const std::vector<Workspace>& workspaces, const std::vector<WorkspaceWindowAssignment>& assignments
  );

  void enrichAssignmentsWithMinimizedState(
      std::vector<WorkspaceWindowAssignment>& assignments, const std::vector<WlrToplevelSnapshot>& minimizedToplevels
  );

} // namespace compositors
