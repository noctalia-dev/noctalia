#include "shell/bar/widgets/workspaces_widget.h"
#include "tests/test_check.h"

#include <string>
#include <utility>

class WorkspacesWidgetTestAccess {
public:
  static std::string tooltip(std::string name, const std::string& label, bool showLabel) {
    return WorkspacesWidget::workspaceTooltipText(Workspace{.name = std::move(name)}, label, showLabel);
  }

  static FontWeight labelWeight(FontWeight configured, bool boldActive, bool minimal, bool active) {
    return WorkspacesWidget::workspaceLabelFontWeight(configured, boldActive, minimal, active);
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

  // Bold active labels are a minimal-style option and never lighten the configured weight.
  TEST_CHECK(WorkspacesWidgetTestAccess::labelWeight(FontWeight::Medium, true, true, true) == FontWeight::Bold);
  TEST_CHECK(WorkspacesWidgetTestAccess::labelWeight(FontWeight::Medium, true, true, false) == FontWeight::Medium);
  TEST_CHECK(WorkspacesWidgetTestAccess::labelWeight(FontWeight::Medium, false, true, true) == FontWeight::Medium);
  TEST_CHECK(WorkspacesWidgetTestAccess::labelWeight(FontWeight::Medium, true, false, true) == FontWeight::Medium);
  TEST_CHECK(WorkspacesWidgetTestAccess::labelWeight(FontWeight::Heavy, true, true, true) == FontWeight::Heavy);

  return 0;
}
