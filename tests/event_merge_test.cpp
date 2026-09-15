#include "calendar/calendar_types.h"
#include "calendar/event_merge.h"

#include <chrono>
#include <map>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace {

  using namespace std::chrono;

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "event_merge_test: {}", message);
    }
    return condition;
  }

  system_clock::time_point minute(int n) { return system_clock::time_point{minutes{n}}; }

  CalendarEvent event(std::string calendarName, std::string title, int startMinute, int endMinute) {
    CalendarEvent e;
    e.id = calendarName + ":" + title;
    e.title = std::move(title);
    e.calendarName = std::move(calendarName);
    e.start = minute(startMinute);
    e.end = minute(endMinute);
    return e;
  }

  std::vector<std::string> titles(const std::vector<CalendarEvent>& events) {
    std::vector<std::string> out;
    out.reserve(events.size());
    for (const auto& e : events) {
      out.push_back(e.title);
    }
    return out;
  }

} // namespace

int main() {
  bool ok = true;

  // Every calendar's events are kept and ordered by start when dedupe is off.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Standup", 60, 90), event("Team", "Lunch", 180, 210)};
    byAccount["personal"] = {event("Shared", "Standup", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, false);
    ok = expect(
             titles(merged) == std::vector<std::string>{"Standup", "Standup", "Lunch"},
             "duplicates were dropped while disabled"
         )
        && ok;
  }

  // An occurrence carried by two calendars collapses to one entry when dedupe is on.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Standup", 60, 90), event("Team", "Lunch", 180, 210)};
    byAccount["personal"] = {event("Shared", "Standup", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    ok = expect(titles(merged) == std::vector<std::string>{"Standup", "Lunch"}, "shared occurrence was not collapsed")
        && ok;
  }

  // The kept copy is the one carrying the most presentation metadata.
  {
    CalendarEvent rich = event("Shared", "Standup", 60, 90);
    rich.colorHex = "#3367d6";
    rich.location = "Room 3";
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Standup", 60, 90)};
    byAccount["personal"] = {rich};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    ok = expect(merged.size() == 1 && merged.front().colorHex == "#3367d6", "richer copy was not kept") && ok;
  }

  // Same title at a different time is a distinct occurrence.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Standup", 60, 90)};
    byAccount["personal"] = {event("Shared", "Standup", 1500, 1530)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    ok = expect(merged.size() == 2, "distinct start times were merged") && ok;
  }

  // An all-day event does not merge with a timed event that shares its title and bounds.
  {
    CalendarEvent allDay = event("Shared", "Release", 0, 1440);
    allDay.allDay = true;
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Release", 0, 1440)};
    byAccount["personal"] = {allDay};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    ok = expect(merged.size() == 2, "all-day and timed occurrences were merged") && ok;
  }

  // Titles that differ only by an ignore-pattern match merge, and the longer title is displayed.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Sprint", 60, 90)};
    byAccount["personal"] = {event("Shared", "Sprint (extended)", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true, {"\\s*\\(.*\\)$"});
    ok = expect(
             merged.size() == 1 && merged.front().title == "Sprint (extended)",
             "ignore pattern did not merge to the detailed title"
         )
        && ok;
  }

  // Without the pattern the same two titles stay separate.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Sprint", 60, 90)};
    byAccount["personal"] = {event("Shared", "Sprint (extended)", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    ok = expect(merged.size() == 2, "differing titles merged without an ignore pattern") && ok;
  }

  // An invalid pattern is skipped rather than throwing.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Standup", 60, 90)};
    byAccount["personal"] = {event("Shared", "Standup", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true, {"("});
    ok = expect(merged.size() == 1, "invalid pattern broke an otherwise exact match") && ok;
  }

  return ok ? 0 : 1;
}
