#include "calendar/calendar_discovery_state.h"

#include "core/log.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace calendar {

  namespace {
    constexpr Logger kLog("calendar-discovery");
  }

  std::string serializeCalendarSources(const std::vector<CalendarSource>& sources) {
    nlohmann::json items = nlohmann::json::array();
    for (const CalendarSource& source : sources) {
      if (source.id.empty()) {
        continue;
      }
      items.push_back({
          {"id", source.id},
          {"name", source.name},
      });
    }
    return nlohmann::json{{"calendars", std::move(items)}}.dump();
  }

  std::vector<CalendarSource> parseCalendarSources(std::string_view value) {
    std::vector<CalendarSource> sources;
    if (value.empty()) {
      return sources;
    }

    try {
      const auto root = nlohmann::json::parse(value);
      const auto calendars = root.find("calendars");
      if (calendars == root.end() || !calendars->is_array()) {
        return sources;
      }

      for (const auto& item : *calendars) {
        CalendarSource source;
        source.id = item.value("id", std::string{});
        source.name = item.value("name", std::string{});
        if (!source.id.empty()) {
          sources.push_back(std::move(source));
        }
      }
    } catch (const std::exception& e) {
      kLog.warn("calendar discovery state parse error: {}", e.what());
    }
    return sources;
  }

  std::vector<std::string>
  selectedCalendarSourceIds(const std::vector<CalendarSource>& sources, const std::vector<std::string>& selectedIds) {
    if (selectedIds.empty()) {
      std::vector<std::string> ids;
      ids.reserve(sources.size());
      for (const CalendarSource& source : sources) {
        if (!source.id.empty()) {
          ids.push_back(source.id);
        }
      }
      return ids;
    }

    const std::unordered_set<std::string> selected(selectedIds.begin(), selectedIds.end());
    std::vector<std::string> ids;
    ids.reserve(sources.size());
    for (const CalendarSource& source : sources) {
      if (selected.contains(source.id)) {
        ids.push_back(source.id);
      }
    }
    return ids;
  }

  std::vector<std::string> setCalendarSourceChecked(
      const std::vector<CalendarSource>& sources, const std::vector<std::string>& selectedIds,
      const std::string& sourceId, bool checked
  ) {
    if (sources.empty()) {
      return selectedIds;
    }

    std::vector<std::string> next = selectedIds;
    if (next.empty()) {
      next = selectedCalendarSourceIds(sources, {});
    }

    if (checked) {
      if (!std::ranges::contains(next, sourceId)) {
        next.push_back(sourceId);
      }
    } else {
      std::erase(next, sourceId);
      if (next.empty()) {
        next.push_back(sourceId);
      }
    }

    const bool allDiscoveredSelected = std::ranges::all_of(sources, [&](const CalendarSource& source) {
      return std::ranges::contains(next, source.id);
    });
    if (allDiscoveredSelected) {
      next.clear();
    }
    return next;
  }

} // namespace calendar
