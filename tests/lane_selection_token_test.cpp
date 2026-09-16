#include "shell/settings/bar_widget_editor.h"
#include "tests/test_check.h"

#include <string>
#include <vector>

namespace {

  using settings::makeLaneSelectionToken;
  using settings::parseLaneSelectionToken;
  using settings::reindexLaneSelectionAfterRemoval;

  void testTokenRoundTrip() {
    TEST_CHECK(makeLaneSelectionToken("start", 0) == "start#0");
    TEST_CHECK(makeLaneSelectionToken("end", 12) == "end#12");

    const auto parsed = parseLaneSelectionToken("center#3");
    TEST_CHECK(parsed.has_value());
    TEST_CHECK(parsed->laneKey == "center");
    TEST_CHECK(parsed->index == 3);
  }

  void testMalformedTokensAreRejected() {
    // A token that does not parse must not be mistaken for lane position 0.
    TEST_CHECK(!parseLaneSelectionToken("start").has_value());
    TEST_CHECK(!parseLaneSelectionToken("").has_value());
    TEST_CHECK(!parseLaneSelectionToken("#2").has_value());
    TEST_CHECK(!parseLaneSelectionToken("start#").has_value());
    TEST_CHECK(!parseLaneSelectionToken("start#x").has_value());
    TEST_CHECK(!parseLaneSelectionToken("start#2x").has_value());
    TEST_CHECK(!parseLaneSelectionToken("start#-1").has_value());
    TEST_CHECK(!parseLaneSelectionToken("start# 2").has_value());
  }

  void testReindexAfterRemoval() {
    // Removing lane position 1: it drops out, higher positions shift down, lower ones stay.
    std::vector<std::string> selection = {"start#0", "start#1", "start#2", "start#4"};
    reindexLaneSelectionAfterRemoval(selection, "start", 1);
    TEST_CHECK(selection == std::vector<std::string>({"start#0", "start#1", "start#3"}));
  }

  void testReindexLeavesOtherLanesAlone() {
    std::vector<std::string> selection = {"center#2", "start#3", "end#0"};
    reindexLaneSelectionAfterRemoval(selection, "start", 0);
    TEST_CHECK(selection == std::vector<std::string>({"center#2", "start#2", "end#0"}));
  }

  void testReindexKeepsUnrelatedEntries() {
    // Nothing addresses the edited lane: the selection is untouched.
    std::vector<std::string> selection = {"end#1", "end#2"};
    reindexLaneSelectionAfterRemoval(selection, "start", 0);
    TEST_CHECK(selection == std::vector<std::string>({"end#1", "end#2"}));

    // Unparseable entries cannot address a card, so they are carried through rather than reinterpreted.
    std::vector<std::string> malformed = {"start", "start#1"};
    reindexLaneSelectionAfterRemoval(malformed, "start", 1);
    TEST_CHECK(malformed == std::vector<std::string>({"start"}));

    std::vector<std::string> empty;
    reindexLaneSelectionAfterRemoval(empty, "start", 0);
    TEST_CHECK(empty.empty());
  }

} // namespace

int main() {
  testTokenRoundTrip();
  testMalformedTokensAreRejected();
  testReindexAfterRemoval();
  testReindexLeavesOtherLanesAlone();
  testReindexKeepsUnrelatedEntries();
  return 0;
}
