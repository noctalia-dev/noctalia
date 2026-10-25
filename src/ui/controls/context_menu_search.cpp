#include "ui/controls/context_menu_search.h"

#include "util/fuzzy_match.h"
#include "util/string_utils.h"

namespace ContextMenuSearch {

  void appendCodepoint(std::string& query, std::uint32_t codepoint) { query += StringUtils::utf32ToUtf8(codepoint); }

  void eraseLastCodepoint(std::string& query) {
    while (!query.empty() && (static_cast<unsigned char>(query.back()) & 0xC0U) == 0x80U) {
      query.pop_back();
    }
    if (!query.empty()) {
      query.pop_back();
    }
  }

  std::vector<std::size_t>
  matchingEntryIndices(const std::vector<ContextMenuControlEntry>& entries, std::string_view query) {
    std::vector<std::size_t> matches;
    if (query.empty()) {
      return matches;
    }

    matches.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const ContextMenuControlEntry& entry = entries[i];
      if (!entry.enabled || entry.separator || entry.header) {
        continue;
      }
      if (FuzzyMatch::isMatch(FuzzyMatch::score(query, entry.label))) {
        matches.push_back(i);
      }
    }
    return matches;
  }

} // namespace ContextMenuSearch
