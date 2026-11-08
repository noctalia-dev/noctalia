#include "compositors/wayfire/wayfire_workspace_backend.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {

  // A 3x3 grid named row-major from 1, with ext-workspace coordinates (row, column).
  std::vector<Workspace> grid() {
    std::vector<Workspace> workspaces;
    for (std::uint32_t row = 0; row < 3; ++row) {
      for (std::uint32_t column = 0; column < 3; ++column) {
        Workspace workspace;
        workspace.id = "HDMI-A-1:" + std::to_string(column) + "," + std::to_string(row);
        workspace.name = std::to_string(row * 3 + column + 1);
        workspace.coordinates = {row, column};
        workspaces.push_back(workspace);
      }
    }
    return workspaces;
  }

  // A 3x2 grid the way Wayfire's own ext-workspace plugin offers it: named
  // "<output>:workspace-<column>-<row>", with coordinates (column, row).
  std::vector<Workspace> upstreamGrid() {
    std::vector<Workspace> workspaces;
    for (std::uint32_t row = 0; row < 2; ++row) {
      for (std::uint32_t column = 0; column < 3; ++column) {
        Workspace workspace;
        workspace.id = "HDMI-A-1:workspace-" + std::to_string(column) + "-" + std::to_string(row);
        workspace.name = workspace.id;
        workspace.coordinates = {column, row};
        workspaces.push_back(workspace);
      }
    }
    return workspaces;
  }

  std::string keyOf(const std::vector<WorkspaceWindow>& windows, zwlr_foreign_toplevel_handle_v1* handle) {
    const std::string id = toplevel_identity::wlr(reinterpret_cast<std::uintptr_t>(handle));
    for (const auto& window : windows) {
      if (window.windowId == id) {
        return window.workspaceKey;
      }
    }
    return {};
  }

  std::string view(int id, const std::string& appId, const std::string& title, int x, int y, bool sticky = false) {
    return R"({"id":)"
        + std::to_string(id)
        + R"(,"role":"toplevel","mapped":true,"app-id":")"
        + appId
        + R"(","title":")"
        + title
        + R"(","output-name":"HDMI-A-1","sticky":)"
        + (sticky ? "true" : "false")
        + R"(,"geometry":{"x":)"
        + std::to_string(x)
        + R"(,"y":)"
        + std::to_string(y)
        + R"(,"width":800,"height":600}})";
  }

} // namespace

int main() {
  // Never reach a real compositor from the test.
  ::unsetenv("WAYFIRE_SOCKET");
  WayfireWorkspaceBackend backend;

  static int storage[6] = {};
  auto* sameTitleFirst = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&storage[0]);
  auto* sameTitleSecond = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&storage[1]);
  auto* left = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&storage[2]);
  auto* sticky = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&storage[3]);
  auto* farDown = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&storage[4]);
  auto* noView = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(&storage[5]);

  std::vector<WlrToplevelSnapshot> toplevels = {
      // Listed out of order: the join must follow `order`, not enumeration order.
      {.handle = sameTitleSecond, .title = "shell", .appId = "foot", .order = 2},
      {.handle = sameTitleFirst, .title = "shell", .appId = "foot", .order = 1},
      {.handle = left, .title = "left", .appId = "foot", .order = 3},
      {.handle = sticky, .title = "pinned", .appId = "foot", .order = 4},
      {.handle = farDown, .title = "down", .appId = "foot", .minimized = true, .order = 5},
      {.handle = noView, .title = "gone", .appId = "foot", .order = 6},
  };
  bool upstream = false;
  backend.setProviders(
      [&](const std::string&) { return upstream ? upstreamGrid() : grid(); },
      [&](const std::function<void(const WlrToplevelSnapshot&)>& visit) {
        for (const auto& toplevel : toplevels) {
          visit(toplevel);
        }
      }
  );

  // The output shows the workspace in column 1, row 0 ("2").
  backend.parseOutputs(
      R"([{"name":"HDMI-A-1","geometry":{"x":0,"y":0,"width":1920,"height":1080},)"
      R"("workspace":{"x":1,"y":0,"grid_width":3,"grid_height":3}}])"
  );
  backend.parseViews(
      "["
      + view(30, "foot", "shell", 1920 + 40, 40)
      + ","
      + view(10, "foot", "shell", 100, 100)
      + ","
      + view(40, "foot", "left", -1900, 100)
      + ","
      + view(50, "foot", "pinned", 5000, 100, true)
      + ","
      + view(60, "foot", "down", 100, 1080 * 5)
      + R"(,{"id":70,"role":"desktop-environment","title":"bar"}])"
  );

  const auto windows = backend.workspaceWindows("HDMI-A-1");
  TEST_CHECK(windows.size() == 5);
  // Same title: the older view (id 10) is the older handle.
  TEST_CHECK(keyOf(windows, sameTitleFirst) == "2");
  TEST_CHECK(keyOf(windows, sameTitleSecond) == "3");
  // Geometry left of the current workspace is the column before it.
  TEST_CHECK(keyOf(windows, left) == "1");
  // Sticky windows belong to whatever workspace is shown.
  TEST_CHECK(keyOf(windows, sticky) == "2");
  // Positions past the grid clamp to its last row.
  TEST_CHECK(keyOf(windows, farDown) == "8");
  // A handle without a matching view gets no workspace rather than a guess.
  TEST_CHECK(keyOf(windows, noView).empty());
  // Ordered like the wlr list, so title occurrences line up with the taskbar's.
  TEST_CHECK(
      !windows.empty()
      && windows.front().windowId == toplevel_identity::wlr(reinterpret_cast<std::uintptr_t>(sameTitleFirst))
  );
  for (const auto& window : windows) {
    if (window.windowId == toplevel_identity::wlr(reinterpret_cast<std::uintptr_t>(farDown))) {
      TEST_CHECK(window.minimizeState.has_value() && window.minimizeState->minimized);
    }
  }

  TEST_CHECK(backend.workspaceWindows("DP-1").empty());
  TEST_CHECK(backend.workspaceKeys("HDMI-A-1").size() == 9 && backend.workspaceKeys("HDMI-A-1").front() == "1");

  // Wayfire's own plugin: cells come from the names, so a 3x2 grid isn't read transposed.
  upstream = true;
  backend.parseOutputs(
      R"([{"name":"HDMI-A-1","geometry":{"x":0,"y":0,"width":1920,"height":1080},)"
      R"("workspace":{"x":1,"y":0,"grid_width":3,"grid_height":2}}])"
  );
  const auto upstreamWindows = backend.workspaceWindows("HDMI-A-1");
  TEST_CHECK(upstreamWindows.size() == 5);
  TEST_CHECK(keyOf(upstreamWindows, sameTitleFirst) == "HDMI-A-1:workspace-1-0");
  TEST_CHECK(keyOf(upstreamWindows, sameTitleSecond) == "HDMI-A-1:workspace-2-0");
  TEST_CHECK(keyOf(upstreamWindows, left) == "HDMI-A-1:workspace-0-0");
  TEST_CHECK(keyOf(upstreamWindows, farDown) == "HDMI-A-1:workspace-1-1");
  upstream = false;
  backend.parseOutputs(
      R"([{"name":"HDMI-A-1","geometry":{"x":0,"y":0,"width":1920,"height":1080},)"
      R"("workspace":{"x":1,"y":0,"grid_width":3,"grid_height":3}}])"
  );

  // A second view with the same title but no second handle yet: that group waits.
  toplevels.erase(toplevels.begin());
  TEST_CHECK(keyOf(backend.workspaceWindows("HDMI-A-1"), sameTitleFirst).empty());
  TEST_CHECK(keyOf(backend.workspaceWindows("HDMI-A-1"), left) == "1");

  return 0;
}
