#include "compositors/workspace_alert_service.h"

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

  // Mixed identifier shapes: numbered rows (id == name == index), a Sway-style
  // named row (id == name, no numeric index), and a niri-style row that only
  // carries a visible index (id and name empty).
  std::vector<Workspace> sampleWorkspaces() {
    return {
        Workspace{.id = "1", .name = "1", .index = 1, .active = false, .urgent = false, .occupied = true},
        Workspace{.id = "2", .name = "2", .index = 2, .active = false, .urgent = false, .occupied = true},
        Workspace{.id = "3", .name = "3", .index = 3, .active = true, .urgent = false, .occupied = true},
        Workspace{.id = "web", .name = "web", .index = 0, .active = false, .urgent = true, .occupied = true},
        Workspace{.id = "", .name = "", .index = 5, .active = false, .urgent = false, .occupied = true},
    };
  }

} // namespace

int main() {
  bool ok = true;

  WorkspaceAlertService service;
  ok &= check(service.empty(), "new service is empty");
  ok &= check(service.clearActive(sampleWorkspaces()) == 0, "empty service clearActive is a no-op");
  ok &= check(service.add("2"), "add returns true for new token");
  ok &= check(!service.add("2"), "duplicate add returns false");
  ok &= check(!service.add(""), "empty add is rejected");
  ok &= check(service.contains("2"), "contains added token");

  auto overlaid = sampleWorkspaces();
  service.applyOverlay(overlaid);
  ok &= check(overlaid[1].urgent, "overlay marks non-active alerted workspace urgent");
  ok &= check(!overlaid[2].urgent, "overlay does not mark unrelated active workspace urgent");
  ok &= check(overlaid[3].urgent, "overlay preserves compositor urgent state");
  ok &= check(!overlaid[4].urgent, "overlay leaves unmatched workspaces alone");
  ok &= check(service.contains("2"), "overlay is pure read and keeps alert");

  // niri-style: a workspace with empty id/name is alertable by its visible index.
  WorkspaceAlertService indexService;
  ok &= check(indexService.add("5"), "index-only token add succeeds");
  auto indexRows = sampleWorkspaces();
  indexService.applyOverlay(indexRows);
  ok &= check(indexRows[4].urgent, "overlay matches workspace by visible index");

  // Sway-style: a named workspace is alertable by its name/id.
  WorkspaceAlertService nameService;
  ok &= check(nameService.add("web"), "name token add succeeds");
  auto nameRows = sampleWorkspaces();
  nameService.applyOverlay(nameRows);
  ok &= check(nameRows[3].urgent, "overlay matches workspace by name/id");

  // The overlay must never mark the active workspace, even if alerted directly.
  WorkspaceAlertService activeService;
  ok &= check(activeService.add("3"), "add active token at service level");
  auto activeOverlay = sampleWorkspaces();
  activeService.applyOverlay(activeOverlay);
  ok &= check(!activeOverlay[2].urgent, "overlay never marks active workspace urgent");

  // Per-output duplicate indexes (e.g. niri/mango/dwl): the same alert token must
  // mark the inactive copy on one output while leaving the active copy alone.
  WorkspaceAlertService dupService;
  ok &= check(dupService.add("2"), "duplicate-index alert add succeeds");
  std::vector<Workspace> outputA{Workspace{.id = "", .name = "", .index = 2, .active = true, .occupied = true}};
  std::vector<Workspace> outputB{Workspace{.id = "", .name = "", .index = 2, .active = false, .occupied = true}};
  dupService.applyOverlay(outputA);
  dupService.applyOverlay(outputB);
  ok &= check(!outputA[0].urgent, "active duplicate index stays unmarked");
  ok &= check(outputB[0].urgent, "inactive duplicate index is alerted");

  // clearActive removes whichever token form named the now-active workspace.
  ok &= check(service.add("3"), "active token add succeeds");
  auto activeRows = sampleWorkspaces();
  const std::size_t cleared = service.clearActive(activeRows);
  ok &= check(cleared == 1, "clearActive clears alert on active workspace");
  ok &= check(!service.contains("3"), "active token removed");
  ok &= check(service.contains("2"), "inactive token remains");

  std::vector<WorkspaceWindowAssignment> assignments{
      WorkspaceWindowAssignment{.windowId = "10", .workspaceKey = "1", .appId = "kitty", .title = "shell"},
      WorkspaceWindowAssignment{.windowId = "20", .workspaceKey = "2", .appId = "code", .title = "editor"},
      WorkspaceWindowAssignment{.windowId = "30", .workspaceKey = "", .appId = "empty", .title = "empty"},
  };
  ok &= check(WorkspaceAlertService::workspaceTokenForWindow("20", assignments) == "2", "resolves window id");
  ok &= check(
      !WorkspaceAlertService::workspaceTokenForWindow("30", assignments).has_value(), "rejects empty assignment key"
  );
  ok &= check(
      !WorkspaceAlertService::workspaceTokenForWindow("99", assignments).has_value(), "rejects unknown window id"
  );

  const auto rows = sampleWorkspaces();
  ok &= check(WorkspaceAlertService::isKnownWorkspaceToken("2", rows), "known token by id/index validates");
  ok &= check(WorkspaceAlertService::isKnownWorkspaceToken("5", rows), "known token by index-only validates");
  ok &= check(WorkspaceAlertService::isKnownWorkspaceToken("web", rows), "known token by name validates");
  ok &= check(!WorkspaceAlertService::isKnownWorkspaceToken("9", rows), "unknown token rejects");
  ok &= check(!WorkspaceAlertService::isKnownWorkspaceToken("0", rows), "zero index never matches");
  ok &= check(!WorkspaceAlertService::isKnownWorkspaceToken("", rows), "empty token rejects");

  ok &= check(service.add("1"), "sort token add succeeds");
  const auto tokens = service.tokens();
  ok &= check(tokens.size() == 2 && tokens[0] == "1" && tokens[1] == "2", "tokens are sorted");
  ok &= check(service.clear("1"), "clear returns true for present token");
  ok &= check(!service.contains("1"), "clear removes token");
  service.clearAll();
  ok &= check(service.empty(), "clearAll empties service");

  return ok ? 0 : 1;
}
