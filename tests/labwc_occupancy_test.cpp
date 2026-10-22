#include "compositors/labwc/labwc_workspace_backend.h"
#include "tests/test_check.h"

#include <functional>
#include <string>
#include <vector>

namespace {

  Workspace makeWorkspace(const std::string& id, bool active) {
    Workspace workspace;
    workspace.id = id;
    workspace.name = id;
    workspace.active = active;
    return workspace;
  }

  [[nodiscard]] bool isOccupied(const std::vector<Workspace>& workspaces, const std::string& id) {
    for (const auto& workspace : workspaces) {
      if (workspace.id == id) {
        return workspace.occupied;
      }
    }
    return false;
  }

} // namespace

int main() {
  LabwcWorkspaceBackend backend;

  std::vector<Workspace> currentWorkspaces;
  std::vector<WlrToplevelSnapshot> currentToplevels;

  backend.setProviders(
      [&]() -> std::vector<Workspace> { return currentWorkspaces; },
      [&](const std::function<void(const WlrToplevelSnapshot&)>& visit) {
        for (const auto& toplevel : currentToplevels) {
          visit(toplevel);
        }
      }
  );

  static int handleA = 0;
  static int handleB = 0;
  static int handleC = 0;
  static int outputOne = 0;
  auto* fakeA = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&handleA);
  auto* fakeB = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&handleB);
  auto* fakeC = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&handleC);
  auto* fakeOutput = reinterpret_cast<wl_output*>(&outputOne);

  const auto visible = [&](auto* handle, const std::string& appId, const std::string& title) {
    return WlrToplevelSnapshot{
        .handle = handle,
        .title = title,
        .appId = appId,
        .output = fakeOutput,
        .activated = false,
        .minimized = false,
    };
  };
  const auto hidden = [&](auto* handle, const std::string& appId, const std::string& title) {
    return WlrToplevelSnapshot{
        .handle = handle,
        .title = title,
        .appId = appId,
        .output = nullptr,
        .activated = false,
        .minimized = false,
    };
  };

  // 1. Window A visible on ws1: only ws1 occupied.
  currentWorkspaces = {makeWorkspace("1", true), makeWorkspace("2", false), makeWorkspace("3", false)};
  currentToplevels = {visible(fakeA, "app-a", "A")};
  TEST_CHECK(backend.sync());
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(isOccupied(workspaces, "1"));
    TEST_CHECK(!isOccupied(workspaces, "2"));
    TEST_CHECK(!isOccupied(workspaces, "3"));
  }
  TEST_CHECK(!backend.sync());

  // 2. Switch to ws2: A is hidden (output == nullptr) but ws1 stays occupied.
  currentWorkspaces = {makeWorkspace("1", false), makeWorkspace("2", true), makeWorkspace("3", false)};
  currentToplevels = {hidden(fakeA, "app-a", "A")};
  (void)backend.sync();
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(isOccupied(workspaces, "1"));
    TEST_CHECK(!isOccupied(workspaces, "2"));
    TEST_CHECK(!isOccupied(workspaces, "3"));
    const auto windows = backend.workspaceWindows();
    TEST_CHECK(windows.size() == 1);
    TEST_CHECK(windows[0].workspaceKey == "1");
  }

  // 3. Open B visible on ws2: ws1 and ws2 occupied.
  currentToplevels = {hidden(fakeA, "app-a", "A"), visible(fakeB, "app-b", "B")};
  TEST_CHECK(backend.sync());
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(isOccupied(workspaces, "1"));
    TEST_CHECK(isOccupied(workspaces, "2"));
    TEST_CHECK(!isOccupied(workspaces, "3"));
  }

  // 4. Switch to ws3: both hidden, both remembered.
  currentWorkspaces = {makeWorkspace("1", false), makeWorkspace("2", false), makeWorkspace("3", true)};
  currentToplevels = {hidden(fakeA, "app-a", "A"), hidden(fakeB, "app-b", "B")};
  (void)backend.sync();
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(isOccupied(workspaces, "1"));
    TEST_CHECK(isOccupied(workspaces, "2"));
    TEST_CHECK(!isOccupied(workspaces, "3"));
  }

  // 5. Hidden title refresh is picked up without losing occupancy.
  currentToplevels = {hidden(fakeA, "app-a", "A2"), hidden(fakeB, "app-b", "B")};
  TEST_CHECK(backend.sync());
  TEST_CHECK(backend.workspaceWindows().size() == 2);

  // 6. Close A while hidden: ws1 becomes empty, ws2 stays occupied.
  currentToplevels = {hidden(fakeB, "app-b", "B")};
  TEST_CHECK(backend.sync());
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(!isOccupied(workspaces, "1"));
    TEST_CHECK(isOccupied(workspaces, "2"));
  }

  // 7. Back on ws2 with B visible: no spurious change, occupancy intact.
  currentWorkspaces = {makeWorkspace("1", false), makeWorkspace("2", true), makeWorkspace("3", false)};
  currentToplevels = {visible(fakeB, "app-b", "B")};
  (void)backend.sync();
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(!isOccupied(workspaces, "1"));
    TEST_CHECK(isOccupied(workspaces, "2"));
    const auto grouped = backend.appIdsByWorkspace();
    TEST_CHECK(grouped.at("2") == std::vector<std::string>({"app-b"}));
  }

  // 8. Minimized window with no history still occupies the visible desktop.
  currentToplevels = {
      visible(fakeB, "app-b", "B"),
      WlrToplevelSnapshot{
          .handle = fakeC,
          .title = "C",
          .appId = "app-c",
          .output = fakeOutput,
          .activated = false,
          .minimized = true,
      },
  };
  TEST_CHECK(backend.sync());
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(isOccupied(workspaces, "2"));
    TEST_CHECK(backend.workspaceWindows().size() == 2);
  }

  // 9. Transient empty workspace list must not wipe tracking.
  currentWorkspaces = {};
  TEST_CHECK(!backend.sync());
  currentWorkspaces = {makeWorkspace("1", false), makeWorkspace("2", true), makeWorkspace("3", false)};
  (void)backend.sync();
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(isOccupied(workspaces, "2"));
  }

  // 10. Closing the minimized window leaves only B tracked.
  currentToplevels = {visible(fakeB, "app-b", "B")};
  TEST_CHECK(backend.sync());
  TEST_CHECK(backend.workspaceWindows().size() == 1);

  // 11. Focusing B while ws3 is active rebinds it there, even though labwc
  // keeps every output set on hidden desktops.
  currentWorkspaces = {makeWorkspace("1", false), makeWorkspace("2", false), makeWorkspace("3", true)};
  currentToplevels = {
      WlrToplevelSnapshot{
          .handle = fakeB,
          .title = "B",
          .appId = "app-b",
          .output = fakeOutput,
          .activated = true,
          .minimized = false,
      },
  };
  TEST_CHECK(backend.sync());
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(!isOccupied(workspaces, "1"));
    TEST_CHECK(!isOccupied(workspaces, "2"));
    TEST_CHECK(isOccupied(workspaces, "3"));
    const auto windows = backend.workspaceWindows();
    TEST_CHECK(windows.size() == 1);
    TEST_CHECK(windows[0].workspaceKey == "3");
  }

  // 12. Transient double-active during a switch: no rebinding, no tracking
  // of unseen windows, no spurious change.
  static int handleD = 0;
  auto* fakeD = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&handleD);
  currentWorkspaces = {makeWorkspace("1", false), makeWorkspace("2", true), makeWorkspace("3", true)};
  currentToplevels = {
      WlrToplevelSnapshot{
          .handle = fakeB,
          .title = "B",
          .appId = "app-b",
          .output = fakeOutput,
          .activated = true,
          .minimized = false,
      },
      visible(fakeD, "app-d", "D"),
  };
  TEST_CHECK(!backend.sync());
  TEST_CHECK(backend.workspaceWindows().size() == 1);
  TEST_CHECK(backend.workspaceWindows()[0].workspaceKey == "3");

  // 13. Back to single-active: the unseen window is assumed newly opened on
  // the active desktop, the focused one stays bound.
  currentWorkspaces = {makeWorkspace("1", false), makeWorkspace("2", false), makeWorkspace("3", true)};
  TEST_CHECK(backend.sync());
  {
    auto workspaces = currentWorkspaces;
    backend.apply(workspaces);
    TEST_CHECK(isOccupied(workspaces, "3"));
    TEST_CHECK(backend.workspaceWindows().size() == 2);
    const auto grouped = backend.appIdsByWorkspace();
    TEST_CHECK(grouped.at("3").size() == 2);
  }

  return 0;
}
