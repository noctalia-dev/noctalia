#pragma once

#include "compositors/compositor_platform.h"
#include "compositors/workspace_backend.h"
#include "shell/switcher/window_switcher_membership.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

// Smart auto-hide pins a bar or dock visible while the output has nothing on screen and lets it hide
// once something appears. "Something on screen" is the active workspace's unminimised windows plus any
// overlay the compositor presents over it — an overlay (Umbriel's scratchpad) is on screen without being
// a workspace member, so the workspace alone cannot answer. The rule is pure so each backend's answer is
// testable, and the bar and dock reach the same verdict.
namespace shell::smart_auto_hide {

  struct OutputContent {
    std::vector<Workspace> workspaces;
    std::vector<WorkspaceWindowAssignment> assignments;
    // The compositor presents an overlay holding at least one window on this output.
    bool overlayPresented = false;
  };

  // `anyUnminimizedToplevel` answers for backends that publish no per-workspace assignments. It is
  // only called in that case: the answer costs a walk of every running application.
  template <typename AnyUnminimizedToplevel>
  [[nodiscard]] bool outputHasContent(const OutputContent& content, AnyUnminimizedToplevel&& anyUnminimizedToplevel) {
    const auto active =
        std::ranges::find_if(content.workspaces, [](const Workspace& workspace) { return workspace.active; });
    if (active == content.workspaces.end()) {
      // No reported active workspace (hotplug, transition): answer "empty", which keeps the surface
      // revealed instead of flapping.
      return false;
    }
    if (content.overlayPresented) {
      return true;
    }
    for (const auto& assignment : content.assignments) {
      if (assignment.minimizeState.has_value() && assignment.minimizeState->minimized) {
        continue;
      }
      if (switcher_membership::workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
        return true;
      }
    }
    if (!content.assignments.empty()) {
      return false;
    }
    // An unknown toplevel state still falls through to the workspace's own occupancy.
    const std::optional<bool> toplevels = anyUnminimizedToplevel();
    if (toplevels.has_value() && !*toplevels) {
      return false;
    }
    return active->occupied;
  }

  // `anyUnminimizedToplevel` stays a callable so the content checks above can answer without it.
  template <typename AnyUnminimizedToplevel>
  [[nodiscard]] bool
  wantsPinnedVisible(const OutputContent& content, bool overviewOpen, AnyUnminimizedToplevel&& anyUnminimizedToplevel) {
    if (overviewOpen) {
      return true;
    }
    return !outputHasContent(content, std::forward<AnyUnminimizedToplevel>(anyUnminimizedToplevel));
  }

  [[nodiscard]] inline OutputContent outputContent(const CompositorPlatform& platform, wl_output* output) {
    return OutputContent{
        .workspaces = platform.workspaces(output),
        .assignments = platform.workspaceWindowAssignments(output),
        .overlayPresented = platform.hasOnScreenOverlay(output),
    };
  }

  [[nodiscard]] inline bool smartWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output) {
    const bool overviewOpen = platform.hasOverviewState() && platform.isOverviewOpen();
    return wantsPinnedVisible(outputContent(platform, output), overviewOpen, [&platform, output] {
      return platform.hasUnminimizedToplevels(output);
    });
  }

} // namespace shell::smart_auto_hide
