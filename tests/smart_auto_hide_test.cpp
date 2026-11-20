// Smart auto-hide pins the bar or dock visible while the output has nothing on screen. An overlay is
// on screen without being a workspace member, so it has to count as content even though the active
// workspace stays empty.

#include "compositors/umbriel/umbriel_runtime.h"
#include "compositors/umbriel/umbriel_workspace_backend.h"
#include "shell/smart_auto_hide.h"
#include "tests/test_check.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

  using shell::smart_auto_hide::OutputContent;
  using shell::smart_auto_hide::outputHasContent;
  using shell::smart_auto_hide::wantsPinnedVisible;

  constexpr auto kAnyToplevel = [] { return std::optional<bool>{true}; };
  constexpr auto kNoToplevel = [] { return std::optional<bool>{false}; };
  constexpr auto kUnknownToplevel = [] { return std::optional<bool>{}; };

  Workspace workspace(std::string id, std::string name, std::uint32_t index, bool active, bool occupied) {
    return Workspace{
        .id = std::move(id),
        .name = std::move(name),
        .coordinates = {},
        .index = index,
        .active = active,
        .occupied = occupied,
    };
  }

  WorkspaceWindowAssignment assignment(std::string workspaceKey, bool minimized = false) {
    WorkspaceWindowAssignment entry{.windowId = "w1", .workspaceKey = std::move(workspaceKey)};
    if (minimized) {
      entry.minimizeState = WindowMinimizeState{.minimized = true, .minimizable = true};
    }
    return entry;
  }

  OutputContent contentOf(
      std::vector<Workspace> workspaces, std::vector<WorkspaceWindowAssignment> assignments,
      bool overlayPresented = false
  ) {
    return OutputContent{
        .workspaces = std::move(workspaces),
        .assignments = std::move(assignments),
        .overlayPresented = overlayPresented,
    };
  }

  void testEmptyWorkspaceHasNoContent() {
    const auto content = contentOf({workspace("1", "1", 1, true, false)}, {});
    TEST_CHECK(!outputHasContent(content, kAnyToplevel));
    TEST_CHECK(wantsPinnedVisible(content, false, kAnyToplevel));
  }

  void testWindowOnTheActiveWorkspaceIsContent() {
    const auto content = contentOf({workspace("1", "1", 1, true, true)}, {assignment("1")});
    TEST_CHECK(outputHasContent(content, kNoToplevel));
    TEST_CHECK(!wantsPinnedVisible(content, false, kNoToplevel));
  }

  void testWindowOnAnotherWorkspaceLeavesThisOneEmpty() {
    const auto content =
        contentOf({workspace("1", "1", 1, true, false), workspace("2", "2", 2, false, true)}, {assignment("2")});
    TEST_CHECK(!outputHasContent(content, kUnknownToplevel));
    TEST_CHECK(wantsPinnedVisible(content, false, kUnknownToplevel));
  }

  void testMinimizedWindowLeavesTheWorkspaceEmpty() {
    const auto content = contentOf({workspace("1", "1", 1, true, true)}, {assignment("1", true)});
    TEST_CHECK(!outputHasContent(content, kAnyToplevel));
    TEST_CHECK(wantsPinnedVisible(content, false, kAnyToplevel));
  }

  // A scratchpad floats over an otherwise empty workspace. Its members carry no workspace assignment,
  // so the overlay flag is the only evidence that something is on screen.
  void testPresentedOverlayHidesOnAnEmptyWorkspace() {
    const auto content = contentOf({workspace("1", "1", 1, true, false)}, {}, true);
    TEST_CHECK(outputHasContent(content, kAnyToplevel));
    TEST_CHECK(!wantsPinnedVisible(content, false, kAnyToplevel));
  }

  void testOverviewKeepsTheSurfaceVisible() {
    const auto content = contentOf({workspace("1", "1", 1, true, true)}, {assignment("1")}, true);
    TEST_CHECK(wantsPinnedVisible(content, true, kAnyToplevel));
  }

  void testToplevelFallbackWhenTheBackendPublishesNoAssignments() {
    const auto empty = contentOf({workspace("1", "1", 1, true, false)}, {});
    TEST_CHECK(!outputHasContent(empty, kNoToplevel));
    const auto occupied = contentOf({workspace("1", "1", 1, true, true)}, {});
    TEST_CHECK(outputHasContent(occupied, kAnyToplevel));
  }

  void testUnknownToplevelStateFallsBackToOccupancy() {
    TEST_CHECK(!outputHasContent(contentOf({workspace("1", "1", 1, true, false)}, {}), kUnknownToplevel));
    TEST_CHECK(outputHasContent(contentOf({workspace("1", "1", 1, true, true)}, {}), kUnknownToplevel));
  }

  // A presented scratchpad member carries no workspace, so the backend has to read `scratchpad` and
  // `visible` for the overlay to exist at all.
  nlohmann::json
  windowEntry(std::string id, std::string workspace, std::string scratchpad, bool visible, std::string output) {
    return nlohmann::json{
        {"id", std::move(id)},
        {"workspace", std::move(workspace)},
        {"scratchpad", std::move(scratchpad)},
        {"visible", visible},
        {"output", std::move(output)},
        {"app_id", "foot"},
        {"title", "shell"},
        {"active", false},
        {"x", 0},
        {"y", 0},
        {"minimized", false},
        {"minimizable", true},
    };
  }

  void testUmbrielReportsAPresentedScratchpadAsAnOverlay() {
    compositors::umbriel::UmbrielRuntime runtime;
    UmbrielWorkspaceBackend backend(runtime);

    backend.handleEvent("windows", nlohmann::json::array({windowEntry("w1", "", "notes", true, "eDP-1")}));
    TEST_CHECK(backend.hasOnScreenOverlay("eDP-1"));
    TEST_CHECK(!backend.hasOnScreenOverlay("HDMI-A-1"));
    // The taskbar's contract is unchanged: members are not workspace windows.
    TEST_CHECK(backend.workspaceWindows("eDP-1").empty());

    // Hiding the pad flips `visible`, which is the transition Umbriel already reports.
    backend.handleEvent("windows", nlohmann::json::array({windowEntry("w2", "", "notes", false, "eDP-1")}));
    TEST_CHECK(!backend.hasOnScreenOverlay("eDP-1"));

    // A workspace window is not an overlay.
    backend.handleEvent("windows", nlohmann::json::array({windowEntry("w3", "eDP-1:1", "", true, "eDP-1")}));
    TEST_CHECK(!backend.hasOnScreenOverlay("eDP-1"));
    TEST_CHECK(backend.workspaceWindows("eDP-1").size() == 1);
  }

  void testOverlayWithoutAnActiveWorkspaceKeepsTheSurfaceVisible() {
    // Hotplug and transitions report no active workspace; an overlay flag alone must not move the surface.
    TEST_CHECK(!outputHasContent(contentOf({}, {}, true), kAnyToplevel));
    TEST_CHECK(wantsPinnedVisible(contentOf({}, {}, true), false, kAnyToplevel));
  }

  void testNumberedWorkspaceNamesMatchTheirNumber() {
    // Sway publishes "1: web" as both id and name while assignments use "1"; the shared matcher covers it.
    const auto content = contentOf({workspace("1: web", "1: web", 0, true, true)}, {assignment("1")});
    TEST_CHECK(outputHasContent(content, kNoToplevel));
  }

  void testOutputWithoutAnActiveWorkspaceHasNoContent() {
    const auto content = contentOf({workspace("1", "1", 1, false, true)}, {assignment("1")});
    TEST_CHECK(!outputHasContent(content, kAnyToplevel));
    TEST_CHECK(wantsPinnedVisible(content, false, kAnyToplevel));
  }

} // namespace

int main() {
  testEmptyWorkspaceHasNoContent();
  testWindowOnTheActiveWorkspaceIsContent();
  testWindowOnAnotherWorkspaceLeavesThisOneEmpty();
  testMinimizedWindowLeavesTheWorkspaceEmpty();
  testPresentedOverlayHidesOnAnEmptyWorkspace();
  testOverviewKeepsTheSurfaceVisible();
  testToplevelFallbackWhenTheBackendPublishesNoAssignments();
  testUnknownToplevelStateFallsBackToOccupancy();
  testOutputWithoutAnActiveWorkspaceHasNoContent();
  testOverlayWithoutAnActiveWorkspaceKeepsTheSurfaceVisible();
  testNumberedWorkspaceNamesMatchTheirNumber();
  testUmbrielReportsAPresentedScratchpadAsAnOverlay();
  return 0;
}
