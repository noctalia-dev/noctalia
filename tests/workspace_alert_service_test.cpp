#include "compositors/workspace_alert_service.h"
#include "tests/test_check.h"

#include <string>
#include <vector>

namespace {

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
  WorkspaceAlertService service;
  TEST_CHECK(service.empty());
  TEST_CHECK(service.clearActive(sampleWorkspaces()) == 0);
  TEST_CHECK(service.add("2"));
  TEST_CHECK(!service.add("2"));
  TEST_CHECK(!service.add(""));
  TEST_CHECK(service.contains("2"));

  auto overlaid = sampleWorkspaces();
  service.applyOverlay(overlaid);
  TEST_CHECK(overlaid[1].urgent);
  TEST_CHECK(!overlaid[2].urgent);
  TEST_CHECK(overlaid[3].urgent);
  TEST_CHECK(!overlaid[4].urgent);
  TEST_CHECK(service.contains("2"));

  // niri-style: a workspace with empty id/name is alertable by its visible index.
  WorkspaceAlertService indexService;
  TEST_CHECK(indexService.add("5"));
  auto indexRows = sampleWorkspaces();
  indexService.applyOverlay(indexRows);
  TEST_CHECK(indexRows[4].urgent);

  // Sway-style: a named workspace is alertable by its name/id.
  WorkspaceAlertService nameService;
  TEST_CHECK(nameService.add("web"));
  auto nameRows = sampleWorkspaces();
  nameService.applyOverlay(nameRows);
  TEST_CHECK(nameRows[3].urgent);

  // The overlay must never mark the active workspace, even if alerted directly.
  WorkspaceAlertService activeService;
  TEST_CHECK(activeService.add("3"));
  auto activeOverlay = sampleWorkspaces();
  activeService.applyOverlay(activeOverlay);
  TEST_CHECK(!activeOverlay[2].urgent);

  // Per-output duplicate indexes (e.g. niri/mango/dwl): the same alert token must
  // mark the inactive copy on one output while leaving the active copy alone.
  WorkspaceAlertService dupService;
  TEST_CHECK(dupService.add("2"));
  std::vector<Workspace> outputA{Workspace{.id = "", .name = "", .index = 2, .active = true, .occupied = true}};
  std::vector<Workspace> outputB{Workspace{.id = "", .name = "", .index = 2, .active = false, .occupied = true}};
  dupService.applyOverlay(outputA);
  dupService.applyOverlay(outputB);
  TEST_CHECK(!outputA[0].urgent);
  TEST_CHECK(outputB[0].urgent);

  // clearActive removes whichever token form named the now-active workspace.
  TEST_CHECK(service.add("3"));
  auto activeRows = sampleWorkspaces();
  const std::size_t cleared = service.clearActive(activeRows);
  TEST_CHECK(cleared == 1);
  TEST_CHECK(!service.contains("3"));
  TEST_CHECK(service.contains("2"));

  std::vector<WorkspaceWindowAssignment> assignments{
      WorkspaceWindowAssignment{.windowId = "10", .workspaceKey = "1", .appId = "kitty", .title = "shell"},
      WorkspaceWindowAssignment{.windowId = "20", .workspaceKey = "2", .appId = "code", .title = "editor"},
      WorkspaceWindowAssignment{.windowId = "30", .workspaceKey = "", .appId = "empty", .title = "empty"},
  };
  TEST_CHECK(WorkspaceAlertService::workspaceTokenForWindow("20", assignments) == "2");
  TEST_CHECK(!WorkspaceAlertService::workspaceTokenForWindow("30", assignments).has_value());
  TEST_CHECK(!WorkspaceAlertService::workspaceTokenForWindow("99", assignments).has_value());

  const auto rows = sampleWorkspaces();
  TEST_CHECK(WorkspaceAlertService::isKnownWorkspaceToken("2", rows));
  TEST_CHECK(WorkspaceAlertService::isKnownWorkspaceToken("5", rows));
  TEST_CHECK(WorkspaceAlertService::isKnownWorkspaceToken("web", rows));
  TEST_CHECK(!WorkspaceAlertService::isKnownWorkspaceToken("9", rows));
  TEST_CHECK(!WorkspaceAlertService::isKnownWorkspaceToken("0", rows));
  TEST_CHECK(!WorkspaceAlertService::isKnownWorkspaceToken("", rows));

  TEST_CHECK(service.add("1"));
  const auto tokens = service.tokens();
  TEST_CHECK(tokens.size() == 2 && tokens[0] == "1" && tokens[1] == "2");
  TEST_CHECK(service.clear("1"));
  TEST_CHECK(!service.contains("1"));
  service.clearAll();
  TEST_CHECK(service.empty());

  return 0;
}
