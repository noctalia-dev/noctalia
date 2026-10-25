#include "tests/test_check.h"
#include "ui/controls/context_menu_search.h"

#include <string>
#include <vector>

namespace {

  ContextMenuControlEntry labelEntry(std::string label) {
    ContextMenuControlEntry entry;
    entry.label = std::move(label);
    return entry;
  }

} // namespace

int main() {
  std::string query;
  ContextMenuSearch::appendCodepoint(query, 'k');
  ContextMenuSearch::appendCodepoint(query, 'e');
  TEST_CHECK(query == "ke");
  ContextMenuSearch::eraseLastCodepoint(query);
  TEST_CHECK(query == "k");

  // A multibyte codepoint must be removed as one unit.
  ContextMenuSearch::appendCodepoint(query, 0x00E9U);
  TEST_CHECK(query == "k\xC3\xA9");
  ContextMenuSearch::eraseLastCodepoint(query);
  TEST_CHECK(query == "k");
  ContextMenuSearch::eraseLastCodepoint(query);
  TEST_CHECK(query.empty());
  ContextMenuSearch::eraseLastCodepoint(query);
  TEST_CHECK(query.empty());

  const std::vector<ContextMenuControlEntry> entries{
      labelEntry("bjork"),       labelEntry("everforest"), labelEntry("kemuri-rose"),
      labelEntry("kemuri-sumi"), labelEntry("matecito"),
  };

  std::vector<std::size_t> matches = ContextMenuSearch::matchingEntryIndices(entries, "kem");
  TEST_CHECK(matches.size() == 2);
  TEST_CHECK(matches[0] == 2);
  TEST_CHECK(matches[1] == 3);

  // Fuzzy matching is case-insensitive and may skip characters.
  matches = ContextMenuSearch::matchingEntryIndices(entries, "KEMROSE");
  TEST_CHECK(matches.size() == 1);
  TEST_CHECK(matches[0] == 2);

  matches = ContextMenuSearch::matchingEntryIndices(entries, "zzz");
  TEST_CHECK(matches.empty());

  // The caller restores the unfiltered list for an empty query.
  matches = ContextMenuSearch::matchingEntryIndices(entries, "");
  TEST_CHECK(matches.empty());

  // Headers, separators, and disabled rows are never results.
  std::vector<ContextMenuControlEntry> mixed{
      labelEntry("Header"),
      labelEntry("Divider"),
      labelEntry("GitHub"),
      labelEntry("Disabled"),
  };
  mixed[0].header = true;
  mixed[1].separator = true;
  mixed[3].enabled = false;

  matches = ContextMenuSearch::matchingEntryIndices(mixed, "i");
  TEST_CHECK(matches.size() == 1);
  TEST_CHECK(matches[0] == 2);

  return 0;
}
