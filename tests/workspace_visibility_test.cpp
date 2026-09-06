#include "compositors/kde/kwin_window_list.h"
#include "compositors/workspace_visibility.h"
#include "wayland/wayland_toplevels.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  std::vector<Workspace> workspacesWithActive(std::string_view activeKey) {
    return {
        Workspace{.id = "1", .name = "1", .index = 1, .active = activeKey == "1"},
        Workspace{.id = "2", .name = "2", .index = 2, .active = activeKey == "2"},
        Workspace{.id = "web", .name = "web", .index = 0, .active = activeKey == "web"},
    };
  }

  std::string kwinRecord(
      std::string_view uuid,
      std::string_view appId,
      std::string_view title,
      std::string_view desktops = "1",
      std::string_view output = "DP-1",
      bool minimized = false
  ) {
    constexpr char kRecordSeparator = '\x1F';
    constexpr char kFieldSeparator = '\x1e';
    return std::string(uuid)
        + kFieldSeparator
        + std::string(appId)
        + kFieldSeparator
        + std::string(title)
        + kFieldSeparator
        + std::string(desktops)
        + kFieldSeparator
        + std::string(output)
        + kFieldSeparator
        + (minimized ? "1" : "0");
  }

} // namespace

int main() {
  bool ok = true;

  const auto ws1 = workspacesWithActive("1");
  ok &= check(compositors::workspaceKeyMatchesAssignment("1", ws1[0]), "matches workspace id");
  ok &= check(compositors::workspaceKeyMatchesAssignment("web", ws1[2]), "matches workspace name");
  ok &= check(compositors::workspaceKeyMatchesAssignment("2", ws1[1]), "matches inactive workspace key");
  ok &= check(!compositors::workspaceKeyMatchesAssignment("", ws1[0]), "rejects empty assignment key");
  ok &= check(!compositors::workspaceKeyMatchesAssignment("9", ws1[0]), "rejects unknown assignment key");

  ok &= check(
      compositors::activeWorkspaceHasVisibleWindows(
          ws1,
          {WorkspaceWindowAssignment{.windowId = "10", .workspaceKey = "1", .appId = "kitty", .title = "shell"}}
      ),
      "visible window on active workspace counts"
  );
  ok &= check(
      !compositors::activeWorkspaceHasVisibleWindows(
          ws1,
          {WorkspaceWindowAssignment{
              .windowId = "10", .workspaceKey = "1", .appId = "kitty", .title = "shell", .minimized = true}}
      ),
      "all minimized windows on active workspace do not count as visible"
  );
  ok &= check(
      compositors::activeWorkspaceHasVisibleWindows(
          ws1,
          {
              WorkspaceWindowAssignment{
                  .windowId = "10", .workspaceKey = "1", .appId = "kitty", .title = "shell", .minimized = true},
              WorkspaceWindowAssignment{.windowId = "11", .workspaceKey = "1", .appId = "code", .title = "editor"},
          }
      ),
      "mixed minimized and visible windows keep workspace occupied"
  );
  ok &= check(
      !compositors::activeWorkspaceHasVisibleWindows(
          ws1, {WorkspaceWindowAssignment{.windowId = "20", .workspaceKey = "2", .appId = "code", .title = "editor"}}
      ),
      "windows only on other workspaces do not count for active workspace"
  );

  auto emptyAssignments = workspacesWithActive("1");
  emptyAssignments[0].occupied = true;
  ok &= check(
      compositors::activeWorkspaceHasVisibleWindows(emptyAssignments, {}), "empty assignments fall back to occupied"
  );
  emptyAssignments[0].occupied = false;
  ok &= check(
      !compositors::activeWorkspaceHasVisibleWindows(emptyAssignments, {}), "empty unoccupied workspace has no windows"
  );

  std::vector<WorkspaceWindowAssignment> assignments{
      WorkspaceWindowAssignment{.windowId = "42", .workspaceKey = "1", .appId = "kitty", .title = "shell"},
  };
  auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1000);
  compositors::enrichAssignmentsWithMinimizedState(
      assignments,
      {WlrToplevelSnapshot{
          .handle = handle,
          .title = "shell",
          .appId = "kitty",
          .minimized = true,
      }}
  );
  ok &= check(assignments[0].minimized, "enrichment marks minimized by handle id");

  assignments = {WorkspaceWindowAssignment{.windowId = "99", .workspaceKey = "1", .appId = "code", .title = "editor"}};
  compositors::enrichAssignmentsWithMinimizedState(
      assignments,
      {WlrToplevelSnapshot{.title = "editor", .appId = "code", .minimized = true}}
  );
  ok &= check(assignments[0].minimized, "enrichment marks minimized by app id and title");

  constexpr char kRecordSeparator = '\x1F';
  const std::string payload = kwinRecord("42", "kitty", "shell", "1", "DP-1", true)
      + kRecordSeparator
      + kwinRecord("43", "code", "editor", "2", "DP-1", false);
  const auto parsed = compositors::kde::parseWindowListPayload(payload);
  ok &= check(parsed.size() == 2, "kwin payload parses two windows");
  ok &= check(parsed[0].minimized, "kwin payload preserves minimized flag");
  ok &= check(!parsed[1].minimized, "kwin payload preserves non-minimized flag");
  ok &= check(parsed[0].desktopIds == std::vector<std::string>{"1"}, "kwin payload parses desktop ids");

  constexpr char kFieldSeparator = '\x1e';
  const std::string legacyPayload = std::string("44") + kFieldSeparator + "kitty" + kFieldSeparator + "legacy"
      + kFieldSeparator + "1" + kFieldSeparator + "DP-1";
  const auto legacyParsed = compositors::kde::parseWindowListPayload(legacyPayload);
  ok &= check(legacyParsed.size() == 1, "legacy payload without minimized field still parses");
  ok &= check(!legacyParsed[0].minimized, "legacy payload defaults minimized to false");

  return ok ? 0 : 1;
}
