#include "shell/switcher/window_switcher_membership.h"
#include "tests/test_check.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

  using switcher_membership::OutputMembership;

  Workspace workspace(std::string id, std::string name, std::uint32_t index, bool active) {
    return Workspace{
        .id = std::move(id),
        .name = std::move(name),
        .coordinates = {},
        .index = index,
        .active = active,
    };
  }

  WorkspaceWindowAssignment assignment(std::string windowId, std::string workspaceKey) {
    return WorkspaceWindowAssignment{.windowId = std::move(windowId), .workspaceKey = std::move(workspaceKey)};
  }

  // The expected ids go through a function call so the commas stay inside parentheses:
  // TEST_CHECK takes a single argument and braces do not protect them.
  bool listsWindows(const std::vector<OutputMembership>& memberships, std::vector<std::string> expected) {
    std::vector<std::string> ids;
    for (const auto& entry : switcher_membership::assignmentsOnVisibleWorkspaces(memberships)) {
      ids.push_back(entry.windowId);
    }
    return ids == expected;
  }

  void testSwayNumberedWorkspaceNames() {
    const Workspace numbered = workspace("1: web", "1: web", 1, true);
    TEST_CHECK(switcher_membership::workspaceKeyMatchesAssignment("1", numbered));
    TEST_CHECK(switcher_membership::workspaceKeyMatchesAssignment("1: web", numbered));
    TEST_CHECK(!switcher_membership::workspaceKeyMatchesAssignment("2", numbered));
    TEST_CHECK(!switcher_membership::workspaceKeyMatchesAssignment("", numbered));

    const Workspace named = workspace("code", "code", 0, true);
    TEST_CHECK(switcher_membership::workspaceKeyMatchesAssignment("code", named));
    TEST_CHECK(!switcher_membership::workspaceKeyMatchesAssignment("1", named));
  }

  // A named workspace must not be reached through the number of the numbered one beside it.
  void testHyprlandWorkspaceKeys() {
    const Workspace named = workspace("name:alpha", "alpha", 0, true);
    TEST_CHECK(switcher_membership::workspaceKeyMatchesAssignment("name:alpha", named));
    TEST_CHECK(switcher_membership::workspaceKeyMatchesAssignment("alpha", named));
    TEST_CHECK(!switcher_membership::workspaceKeyMatchesAssignment("1", named));

    const Workspace numbered = workspace("8", "8", 0, true);
    TEST_CHECK(switcher_membership::workspaceKeyMatchesAssignment("8", numbered));
    TEST_CHECK(!switcher_membership::workspaceKeyMatchesAssignment("name:8", numbered));
  }

  void testKeepsOnlyTheActiveWorkspace() {
    OutputMembership membership;
    membership.workspaces = {
        workspace("1", "1", 0, false),
        workspace("2", "2", 0, true),
        workspace("3", "3", 0, false),
    };
    membership.assignments = {assignment("w1", "1"), assignment("w2", "2"), assignment("w3", "3")};

    TEST_CHECK(listsWindows({membership}, {"w2"}));
  }

  // The overlay covers the regular workspace, which Hyprland still reports as active.
  void testOverlayWorkspaceReplacesTheActiveWorkspace() {
    OutputMembership membership;
    membership.workspaces = {workspace("8", "8", 0, true)};
    membership.overlayKeys = {"special:magic"};
    membership.assignments = {assignment("behind", "8"), assignment("scratch", "special:magic")};

    TEST_CHECK(listsWindows({membership}, {"scratch"}));
    // Overlay keys live in the assignment namespace: a regular key must not slip through.
    TEST_CHECK(!switcher_membership::assignmentOnVisibleWorkspace(membership, "8"));
  }

  void testOverlayIsScopedToItsOutput() {
    OutputMembership left;
    left.workspaces = {workspace("1", "1", 0, true)};
    left.overlayKeys = {"special:magic"};
    left.assignments = {assignment("left-regular", "1"), assignment("left-scratch", "special:magic")};

    OutputMembership right;
    right.workspaces = {workspace("1", "1", 0, true)};
    right.assignments = {assignment("right-regular", "1")};

    TEST_CHECK(listsWindows({left, right}, {"left-scratch", "right-regular"}));
  }

  // A window tagged onto several workspaces stays listed as long as one of them shows.
  void testWindowOnSeveralWorkspaces() {
    OutputMembership membership;
    membership.workspaces = {workspace("1", "1", 0, false), workspace("2", "2", 0, true)};
    membership.assignments = {assignment("w1", "1"), assignment("w1", "2"), assignment("w2", "2")};

    TEST_CHECK(listsWindows({membership}, {"w1", "w2"}));
  }

  // Mango reports the same tag index on every monitor, so a tag must be judged against
  // the output that owns the window.
  void testIdenticalTagIdsAreJudgedPerOutput() {
    OutputMembership left;
    left.workspaces = {workspace("1", "1", 1, true), workspace("2", "2", 2, false)};
    left.assignments = {assignment("left-1", "1"), assignment("left-2", "2")};

    OutputMembership right;
    right.workspaces = {workspace("1", "1", 1, false), workspace("2", "2", 2, true)};
    right.assignments = {assignment("right-1", "1"), assignment("right-2", "2")};

    TEST_CHECK(listsWindows({left, right}, {"left-1", "right-2"}));
  }

  // Focus only decides when no workspace claims to be active.
  void testFocusIsOnlyAFallback() {
    OutputMembership membership;
    membership.workspaces = {workspace("1", "1", 0, false), workspace("2", "2", 0, true)};
    membership.focusedKey = "1";
    membership.assignments = {assignment("focused", "1"), assignment("active", "2")};
    TEST_CHECK(listsWindows({membership}, {"active"}));

    membership.workspaces = {workspace("1", "1", 0, false), workspace("2", "2", 0, false)};
    TEST_CHECK(listsWindows({membership}, {"focused"}));

    // An unknown focus leaves nothing to show rather than showing every workspace.
    membership.focusedKey.clear();
    TEST_CHECK(switcher_membership::assignmentsOnVisibleWorkspaces({membership}).empty());
  }

  // A backend can report assignments for an output it has no workspace list for: nothing
  // can be filtered there, so every window of that output has to stay.
  void testOutputWithoutWorkspaceModelKeepsItsWindows() {
    OutputMembership membership;
    membership.assignments = {assignment("w1", "1"), assignment("w2", "2")};

    TEST_CHECK(listsWindows({membership}, {"w1", "w2"}));
  }

  void testBackendsWithoutAssignmentsCannotBeFiltered() {
    OutputMembership withoutAssignments;
    withoutAssignments.workspaces = {workspace("1", "1", 0, true)};

    TEST_CHECK(!switcher_membership::hasWorkspaceMembershipData({withoutAssignments}));

    // One output reporting data is enough to turn the filter on everywhere.
    OutputMembership withAssignments;
    withAssignments.assignments = {assignment("w1", "1")};
    TEST_CHECK(switcher_membership::hasWorkspaceMembershipData({withoutAssignments, withAssignments}));
  }

  // Turning the filter on with no data would hide every window instead of none, so an
  // empty membership list must report the filter as unusable.
  void testNoOutputDisablesTheFilter() {
    TEST_CHECK(!switcher_membership::hasWorkspaceMembershipData({}));
    TEST_CHECK(switcher_membership::assignmentsOnVisibleWorkspaces({}).empty());
  }

  void testEntriesWithoutIdentityAreDropped() {
    OutputMembership membership;
    membership.workspaces = {workspace("1", "1", 0, true)};
    membership.assignments = {assignment("", "1"), assignment("w1", ""), assignment("w1", "1")};

    TEST_CHECK(listsWindows({membership}, {"w1"}));
  }

} // namespace

int main() {
  testSwayNumberedWorkspaceNames();
  testHyprlandWorkspaceKeys();
  testKeepsOnlyTheActiveWorkspace();
  testOverlayWorkspaceReplacesTheActiveWorkspace();
  testOverlayIsScopedToItsOutput();
  testWindowOnSeveralWorkspaces();
  testIdenticalTagIdsAreJudgedPerOutput();
  testFocusIsOnlyAFallback();
  testOutputWithoutWorkspaceModelKeepsItsWindows();
  testBackendsWithoutAssignmentsCannotBeFiltered();
  testNoOutputDisablesTheFilter();
  testEntriesWithoutIdentityAreDropped();
  return 0;
}
