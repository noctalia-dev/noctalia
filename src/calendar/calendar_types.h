#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// A single concrete calendar event instance. Recurring events are expanded server-side, so every
// instance carries its own resolved start/end.
struct CalendarEvent {
  std::string id;           // iCal UID / provider event id
  std::string title;        // SUMMARY
  std::string calendarName; // owning calendar's display name
  std::string colorHex;     // owning calendar's color (e.g. "#3367d6"), empty when unknown
  std::string location;     // LOCATION, optional
  std::string url;          // resolved http(s) link from LOCATION/URL, empty when the event has none
  std::chrono::system_clock::time_point start;
  std::chrono::system_clock::time_point end;
  bool allDay = false;
  // Reminder lead times before `start`, in seconds; sorted ascending, deduplicated, 0 = at start.
  // Absolute VALARM triggers and RELATED=END triggers are normalized to a start-relative lead at
  // parse time, so recurrence-expanded instances inherit them unchanged. Empty means the event
  // carries no explicit reminder and the configured default lead applies instead.
  std::vector<std::int32_t> reminderLeadSeconds;
};

struct CalendarSnapshot {
  bool valid = false; // true once at least one successful sync has populated events
  std::vector<CalendarEvent> events;
};

struct CalendarSource {
  std::string id;
  std::string name;

  bool operator==(const CalendarSource&) const = default;
};
