#include "shell/bar/widgets/workspaces_widget.h"
#include "tests/test_check.h"

#include <string>
#include <utility>
#include <vector>

class WorkspacesWidgetTestAccess {
public:
  static std::string tooltip(std::string name, const std::string& label, bool showLabel) {
    return WorkspacesWidget::workspaceTooltipText(Workspace{.name = std::move(name)}, label, showLabel);
  }

  static std::vector<std::string> hoverAppIds(
      const std::vector<WorkspaceWindowAssignment>& assignments, std::string_view workspaceKey,
      const Workspace& workspace, std::size_t limit = 6
  ) {
    return WorkspacesWidget::hoverAppIdsForWorkspace(assignments, workspaceKey, workspace, limit);
  }
};

int main() {
  // A name cut short by max_label_chars is recoverable on hover.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("client portal", "cli", true) == "client portal");
  // A pill that already spells out the whole name has nothing to add.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("web", "web", true).empty());
  // label_source = "id" labels a named workspace by number, so the name is only reachable on hover.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("client portal", "3", true) == "client portal");
  // A hidden label shows nothing, even when its text would have matched the name.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("web", "web", false) == "web");
  // Unnamed workspaces have no tooltip, never an empty one.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("", "3", true).empty());
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("", "", false).empty());

  // Hover app icons follow window position, left to right, so the row does not reshuffle between
  // updates, and a workspace running one app twice still shows one icon.
  const std::vector<WorkspaceWindowAssignment> assignments{
      {.windowId = "c", .workspaceKey = "2", .appId = "firefox", .x = 300},
      {.windowId = "a", .workspaceKey = "2", .appId = "kitty", .x = 100},
      {.windowId = "b", .workspaceKey = "2", .appId = "kitty", .x = 200},
      {.windowId = "d", .workspaceKey = "3", .appId = "mpv", .x = 0},
      {.windowId = "e", .workspaceKey = "2", .appId = "", .x = 50},
  };
  const Workspace second{.id = "2", .index = 2};

  const std::vector<std::string> kittyThenFirefox{"kitty", "firefox"};
  const std::vector<std::string> kittyOnly{"kitty"};

  TEST_CHECK(WorkspacesWidgetTestAccess::hoverAppIds(assignments, "2", second) == kittyThenFirefox);
  // The limit caps how far a hovered pill can grow.
  TEST_CHECK(WorkspacesWidgetTestAccess::hoverAppIds(assignments, "2", second, 1) == kittyOnly);
  // A display key identifies the workspace on its own; the identity match below is only for
  // backends that report no keys.
  const std::vector<std::string> mpvOnly{"mpv"};
  TEST_CHECK(WorkspacesWidgetTestAccess::hoverAppIds(assignments, "3", second) == mpvOnly);
  TEST_CHECK(WorkspacesWidgetTestAccess::hoverAppIds(assignments, "", second) == kittyThenFirefox);
  TEST_CHECK(WorkspacesWidgetTestAccess::hoverAppIds(assignments, "", Workspace{.id = "9", .index = 9}).empty());

  return 0;
}
