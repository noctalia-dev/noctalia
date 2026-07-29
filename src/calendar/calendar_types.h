#pragma once

#include <chrono>
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
  std::chrono::system_clock::time_point start;
  std::chrono::system_clock::time_point end;
  bool allDay = false;
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
