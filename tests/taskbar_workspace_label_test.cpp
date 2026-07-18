#include "shell/bar/widgets/taskbar_workspace_label.h"

#include <cassert>
#include <string>

int main() {
  // Non-KDE: a named workspace (e.g. niri "workspace n") shows its name.
  assert(taskbarWorkspaceLabel(false, 2, "n", "2") == "n");

  // Non-KDE: an unnamed workspace falls back to the numeric index label.
  assert(taskbarWorkspaceLabel(false, 3, "", "3") == "3");

  // Non-KDE: the name wins even when a numeric index is also available.
  assert(taskbarWorkspaceLabel(false, 5, "code", "5") == "code");

  // KDE with a known desktop index ignores the (verbose) desktop name.
  assert(taskbarWorkspaceLabel(true, 1, "Desktop 1", "1") == "1");

  // KDE without a usable index (0) still honors a name.
  assert(taskbarWorkspaceLabel(true, 0, "n", "2") == "n");

  // Nothing usable degrades to the supplied index label.
  assert(taskbarWorkspaceLabel(false, 0, "", "4") == "4");

  // showName=false ignores the name and always uses the numeric label,
  // even when a name is present (keeps long names off the icons).
  assert(taskbarWorkspaceLabel(false, 2, "- really long text -", "2", false) == "2");
  assert(taskbarWorkspaceLabel(false, 5, "code", "5", false) == "5");
  // showName=true is the default and still prefers the name.
  assert(taskbarWorkspaceLabel(false, 2, "n", "2", true) == "n");

  return 0;
}
