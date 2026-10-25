#pragma once

#include "ui/controls/context_menu.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Type-to-filter support for searchable select dropdowns. The query is built from typed
// codepoints and matched against entry labels with the same fuzzy matcher the launcher uses.
namespace ContextMenuSearch {

  // Appends one Unicode codepoint to the query as UTF-8. Callers reject control codepoints.
  void appendCodepoint(std::string& query, std::uint32_t codepoint);

  // Removes the trailing codepoint, including every UTF-8 continuation byte it owns.
  void eraseLastCodepoint(std::string& query);

  // Indices into `entries` whose labels match `query`, in their original order. Headers,
  // separators, and disabled entries never match. An empty query returns nothing; callers
  // restore the unfiltered list themselves.
  [[nodiscard]] std::vector<std::size_t>
  matchingEntryIndices(const std::vector<ContextMenuControlEntry>& entries, std::string_view query);

} // namespace ContextMenuSearch
