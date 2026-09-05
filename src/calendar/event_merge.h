#pragma once

#include "calendar/calendar_types.h"

#include <map>
#include <string>
#include <vector>

namespace calendar {

  // Flatten the per-account event lists into one snapshot ordered by start time.
  //
  // When dedupe is true, an occurrence that appears in more than one calendar is collapsed to a
  // single entry. This covers two calendars in one account that both hold the event as well as the
  // same calendar reached through two accounts (say a Google account and a CalDAV account).
  // Provider event ids differ between backends and between calendars, so identity is the visible
  // shape of the occurrence: start, end, the all-day flag, and the title.
  //
  // ignorePatterns are ECMAScript regexes (case-insensitive); every match is removed from a title
  // before the titles are compared, so two calendars that word the same event slightly differently
  // still count as one occurrence. Invalid patterns are skipped.
  //
  // Among duplicates the entry kept is the one that shows the most: more filled-in fields (color,
  // link, location), then the longer title.
  [[nodiscard]] std::vector<CalendarEvent> mergeCalendarEvents(
      const std::map<std::string, std::vector<CalendarEvent>>& eventsByAccount, bool dedupe,
      const std::vector<std::string>& ignorePatterns = {}
  );

} // namespace calendar
