#pragma once

#include "calendar/calendar_types.h"

#include <string>
#include <string_view>
#include <vector>

namespace calendar {

  [[nodiscard]] std::string serializeCalendarSources(const std::vector<CalendarSource>& sources);
  [[nodiscard]] std::vector<CalendarSource> parseCalendarSources(std::string_view value);
  [[nodiscard]] std::vector<std::string>
  selectedCalendarSourceIds(const std::vector<CalendarSource>& sources, const std::vector<std::string>& selectedIds);
  [[nodiscard]] std::vector<std::string> setCalendarSourceChecked(
      const std::vector<CalendarSource>& sources, const std::vector<std::string>& selectedIds,
      const std::string& sourceId, bool checked
  );

} // namespace calendar
