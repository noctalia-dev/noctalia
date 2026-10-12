#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
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
  std::string webUrl;       // http(s) page of the event in the provider's web UI (Google htmlLink), optional
  std::chrono::system_clock::time_point start;
  std::chrono::system_clock::time_point end;
  bool allDay = false;
  // Reminder lead times before `start`, in seconds; sorted ascending, deduplicated, 0 = at start.
  // Absolute VALARM triggers and RELATED=END triggers are normalized to a start-relative lead at
  // parse time, so recurrence-expanded instances inherit them unchanged. nullopt means the source
  // says nothing about reminders and the configured default lead applies; an empty list means the
  // source explicitly has no reminder for this event (e.g. a Google event with notifications removed).
  std::optional<std::vector<std::int32_t>> reminderLeadSeconds;
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
