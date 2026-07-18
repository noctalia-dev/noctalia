#include "compositors/niri/niri_runtime.h"
#include "compositors/niri/niri_workspace_backend.h"

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

  nlohmann::json workspacesEvent() {
    return {
        {"workspaces",
         {
             {{"id", 1}, {"idx", 1}, {"output", "DP-1"}},
             {{"id", 2}, {"idx", 2}, {"name", "n"}, {"output", "DP-1"}},
             {{"id", 5}, {"idx", 3}, {"output", "DP-2"}},
         }},
    };
  }

  nlohmann::json windowsEvent() {
    return {
        {"windows",
         {
             {{"id", 100}, {"workspace_id", 1}, {"app_id", "kitty"}, {"title", "Terminal"}},
             {{"id", 101}, {"workspace_id", 2}, {"app_id", "firefox"}, {"title", "Web\nPage"}},
             // Same app on the same workspace: must be de-duplicated in appIdsByWorkspace.
             {{"id", 102}, {"workspace_id", 2}, {"app_id", "firefox"}, {"title", "Second"}},
         }},
    };
  }

  const WorkspaceWindow* findWindow(const std::vector<WorkspaceWindow>& windows, const std::string& id) {
    for (const auto& window : windows) {
      if (window.windowId == id) {
        return &window;
      }
    }
    return nullptr;
  }

} // namespace

int main() {
  // apply() gates on runtime availability, which is simply a non-empty NIRI_SOCKET
  // (no real connection is made). Set it so the occupancy/index path can run.
  setenv("NIRI_SOCKET", "/tmp/noctalia-niri-workspace-test.sock", 1);

  compositors::niri::NiriRuntime runtime;
  NiriWorkspaceBackend backend(runtime);

  int changes = 0;
  int overviewChanges = 0;
  backend.setChangeCallback([&changes]() { ++changes; });
  backend.setOverviewChangeCallback([&overviewChanges]() { ++overviewChanges; });

  assert(!backend.hasOverviewState());

  // --- Workspaces -----------------------------------------------------------
  backend.handleEvent("WorkspacesChanged", workspacesEvent());
  assert(changes == 1);

  // Re-sending an identical snapshot is a no-op (no spurious change callback).
  backend.handleEvent("WorkspacesChanged", workspacesEvent());
  assert(changes == 1);

  // Display keys are the niri workspace indices, sorted by idx.
  assert(backend.workspaceKeys() == std::vector<std::string>({"1", "2", "3"}));
  assert(backend.workspaceKeys("DP-1") == std::vector<std::string>({"1", "2"}));
  assert(backend.workspaceKeys("DP-2") == std::vector<std::string>({"3"}));

  // --- Windows --------------------------------------------------------------
  backend.handleEvent("WindowsChanged", windowsEvent());
  assert(changes == 2);

  const auto apps = backend.appIdsByWorkspace("DP-1");
  assert(apps.size() == 2);
  assert(apps.at("1") == std::vector<std::string>({"kitty"}));
  assert(apps.at("2") == std::vector<std::string>({"firefox"})); // de-duplicated
  assert(backend.appIdsByWorkspace("DP-2").empty());

  const auto dp1Windows = backend.workspaceWindows("DP-1");
  assert(dp1Windows.size() == 3);
  const WorkspaceWindow* firefox = findWindow(dp1Windows, "101");
  assert(firefox != nullptr);
  assert(firefox->workspaceKey == "2");
  assert(firefox->appId == "firefox");
  assert(firefox->title == "Web Page"); // newline collapsed to a single space

  // --- apply(): index + occupancy, including name-based matching ------------
  // A named workspace is matched by its name when it carries no numeric id.
  std::vector<Workspace> dp1 = {Workspace{.id = "1"}, Workspace{.name = "n"}};
  backend.apply(dp1, "DP-1");
  assert(dp1[0].index == 1);
  assert(dp1[0].occupied); // window 100 lives here
  assert(dp1[1].index == 2); // matched purely by name "n"
  assert(dp1[1].occupied); // windows 101 + 102 live here

  std::vector<Workspace> dp2 = {Workspace{.id = "5"}};
  backend.apply(dp2, "DP-2");
  assert(dp2[0].index == 3);
  assert(!dp2[0].occupied); // no windows on this workspace

  // Closing the only window on workspace 1 clears its occupancy.
  backend.handleEvent("WindowClosed", nlohmann::json{{"id", 100}});
  assert(changes == 3);
  std::vector<Workspace> dp1After = {Workspace{.id = "1"}, Workspace{.name = "n"}};
  backend.apply(dp1After, "DP-1");
  assert(!dp1After[0].occupied); // window 100 is gone
  assert(dp1After[1].occupied); // 101 + 102 remain

  // --- Overview state -------------------------------------------------------
  backend.handleEvent("OverviewOpenedOrClosed", nlohmann::json{{"is_open", true}});
  assert(overviewChanges == 1);
  assert(backend.hasOverviewState());
  assert(backend.isOverviewOpen());

  backend.handleEvent("OverviewClosed", nlohmann::json::object());
  assert(overviewChanges == 2);
  assert(!backend.isOverviewOpen());

  // The change callback is unaffected by overview transitions.
  assert(changes == 3);

  return 0;
}
