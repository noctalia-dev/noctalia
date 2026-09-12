#pragma once

#include "calendar/calendar_types.h"

#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string_view>
#include <vector>

namespace calendar {

  enum class ICalParseStatus { Complete, Cancelled, WorkBudgetExceeded, InvalidCalendar };

  struct ICalParseControl {
    std::stop_token stopToken;
    std::size_t remainingRecurrenceWork = 100'000;
  };

  struct ICalParseResult {
    std::vector<CalendarEvent> events;
    ICalParseStatus status = ICalParseStatus::Complete;
  };

  // Parse iCalendar (RFC 5545) text into concrete event instances. Callers request server-side
  // recurrence expansion (CalDAV <C:expand>), so a compliant server already returns one VEVENT per
  // occurrence. As a fallback for servers that ignore <C:expand> and return the master VEVENT with an
  // RRULE, that RRULE is expanded client-side, bounded to [windowStart, windowEnd].
  // UID/SUMMARY/DTSTART/DTEND/LOCATION/RRULE/EXDATE are read; VTODO and other components are ignored.
  // VALARM TRIGGERs with ACTION DISPLAY/AUDIO (or no ACTION) become CalendarEvent::reminderLeadSeconds,
  // normalized to seconds before DTSTART. A VALARM's REPEAT/DURATION snooze ladder is ignored, as are
  // absolute triggers on recurring events (they fire once for the series, not per occurrence).

  ICalParseResult parseICalEvents(
      std::string_view ics, std::chrono::system_clock::time_point windowStart,
      std::chrono::system_clock::time_point windowEnd, ICalParseControl& control
  );

} // namespace calendar
