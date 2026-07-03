#pragma once

#include "calendar/calendar_types.h"

#include <chrono>
#include <string_view>
#include <vector>

namespace calendar {

  // Parse iCalendar (RFC 5545) text into concrete event instances. Callers request server-side
  // recurrence expansion (CalDAV <C:expand>), so a compliant server already returns one VEVENT per
  // occurrence. As a fallback for servers that ignore <C:expand> and return the master VEVENT with an
  // RRULE, that RRULE is expanded client-side, bounded to [windowStart, windowEnd].
  // UID/SUMMARY/DTSTART/DTEND/LOCATION/RRULE/EXDATE are read; VTODO/VALARM and others are ignored.
  std::vector<CalendarEvent> parseICalEvents(
      std::string_view ics, std::chrono::system_clock::time_point windowStart,
      std::chrono::system_clock::time_point windowEnd
  );

} // namespace calendar
