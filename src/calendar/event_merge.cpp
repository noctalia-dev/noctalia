#include "calendar/event_merge.h"

#include "util/string_utils.h"

#include <algorithm>
#include <regex>
#include <tuple>
#include <utility>

namespace calendar {

  namespace {
    int metadataRichness(const CalendarEvent& event) {
      return (event.colorHex.empty() ? 0 : 1) + (event.url.empty() ? 0 : 1) + (event.location.empty() ? 0 : 1);
    }

    // A representative is "better" when it shows more: more filled-in fields, then a longer title
    // (the copy that keeps the extra detail rather than the bare version).
    bool showsMore(const CalendarEvent& candidate, const CalendarEvent& current) {
      return std::tuple(metadataRichness(candidate), candidate.title.size())
          > std::tuple(metadataRichness(current), current.title.size());
    }

    bool sameSpan(const CalendarEvent& a, const CalendarEvent& b) {
      return a.allDay == b.allDay && a.start == b.start && a.end == b.end;
    }

    std::vector<std::regex> compileIgnorePatterns(const std::vector<std::string>& patterns) {
      std::vector<std::regex> compiled;
      compiled.reserve(patterns.size());
      for (const auto& pattern : patterns) {
        if (pattern.empty()) {
          continue;
        }
        try {
          compiled.emplace_back(pattern, std::regex_constants::ECMAScript | std::regex_constants::icase);
        } catch (const std::regex_error&) {
          // Skip invalid patterns, same as notification content filters.
        }
      }
      return compiled;
    }

    // Title with every ignore-pattern match removed and whitespace collapsed, so two titles that
    // differ only by an ignored fragment compare equal.
    std::string dedupeKeyTitle(const std::string& title, const std::vector<std::regex>& ignorePatterns) {
      std::string stripped = title;
      for (const auto& pattern : ignorePatterns) {
        stripped = std::regex_replace(stripped, pattern, " ");
      }
      return StringUtils::windowTitleSingleLine(stripped);
    }
  } // namespace

  std::vector<CalendarEvent> mergeCalendarEvents(
      const std::map<std::string, std::vector<CalendarEvent>>& eventsByAccount, bool dedupe,
      const std::vector<std::string>& ignorePatterns
  ) {
    std::vector<CalendarEvent> merged;
    for (const auto& [accountId, events] : eventsByAccount) {
      merged.insert(merged.end(), events.begin(), events.end());
    }

    if (dedupe && merged.size() > 1) {
      const std::vector<std::regex> ignore = compileIgnorePatterns(ignorePatterns);

      std::vector<std::pair<std::string, CalendarEvent>> keyed;
      keyed.reserve(merged.size());
      for (auto& event : merged) {
        keyed.emplace_back(dedupeKeyTitle(event.title, ignore), std::move(event));
      }
      std::ranges::sort(keyed, [](const auto& a, const auto& b) {
        return std::tie(a.second.start, a.second.end, a.second.allDay, a.first)
            < std::tie(b.second.start, b.second.end, b.second.allDay, b.first);
      });

      std::vector<CalendarEvent> deduped;
      deduped.reserve(keyed.size());
      std::string lastKey;
      for (auto& [key, event] : keyed) {
        if (!deduped.empty() && key == lastKey && sameSpan(deduped.back(), event)) {
          if (showsMore(event, deduped.back())) {
            deduped.back() = std::move(event);
          }
          continue;
        }
        lastKey = key;
        deduped.push_back(std::move(event));
      }
      merged = std::move(deduped);
    }

    std::ranges::sort(merged, {}, &CalendarEvent::start);
    return merged;
  }

} // namespace calendar
