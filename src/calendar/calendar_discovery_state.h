#pragma once

#include "calendar/calendar_types.h"

#include <string>
#include <string_view>
#include <vector>

namespace calendar {

  std::string serializeCalendarSources(const std::vector<CalendarSource>& sources);
  std::vector<CalendarSource> parseCalendarSources(std::string_view value);
  std::vector<std::string>
  selectedCalendarSourceIds(const std::vector<CalendarSource>& sources, const std::vector<std::string>& selectedIds);
  std::vector<std::string> setCalendarSourceChecked(
      const std::vector<CalendarSource>& sources, const std::vector<std::string>& selectedIds,
      const std::string& sourceId, bool checked
  );

} // namespace calendar
