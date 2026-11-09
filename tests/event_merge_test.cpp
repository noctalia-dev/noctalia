#include "calendar/calendar_types.h"
#include "calendar/event_merge.h"
#include "tests/test_check.h"

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

  using namespace std::chrono;

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
  // Every calendar's events are kept and ordered by start when dedupe is off.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Standup", 60, 90), event("Team", "Lunch", 180, 210)};
    byAccount["personal"] = {event("Shared", "Standup", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, false);
    TEST_CHECK(titles(merged) == std::vector<std::string>{"Standup", "Standup", "Lunch"});
  }

  // An occurrence carried by two calendars collapses to one entry when dedupe is on.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Standup", 60, 90), event("Team", "Lunch", 180, 210)};
    byAccount["personal"] = {event("Shared", "Standup", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    TEST_CHECK(titles(merged) == std::vector<std::string>{"Standup", "Lunch"});
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
    TEST_CHECK(merged.size() == 1 && merged.front().colorHex == "#3367d6");
  }

  // Same title at a different time is a distinct occurrence.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Standup", 60, 90)};
    byAccount["personal"] = {event("Shared", "Standup", 1500, 1530)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    TEST_CHECK(merged.size() == 2);
  }

  // An all-day event does not merge with a timed event that shares its title and bounds.
  {
    CalendarEvent allDay = event("Shared", "Release", 0, 1440);
    allDay.allDay = true;
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Release", 0, 1440)};
    byAccount["personal"] = {allDay};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    TEST_CHECK(merged.size() == 2);
  }

  // Titles that differ only by an ignore-pattern match merge, and the longer title is displayed.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Sprint", 60, 90)};
    byAccount["personal"] = {event("Shared", "Sprint (extended)", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true, {R"(\s*\(.*\)$)"});
    TEST_CHECK(merged.size() == 1 && merged.front().title == "Sprint (extended)");
  }

  // Without the pattern the same two titles stay separate.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Sprint", 60, 90)};
    byAccount["personal"] = {event("Shared", "Sprint (extended)", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    TEST_CHECK(merged.size() == 2);
  }

  // An invalid pattern is skipped rather than throwing.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    byAccount["work"] = {event("Team", "Standup", 60, 90)};
    byAccount["personal"] = {event("Shared", "Standup", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true, {"("});
    TEST_CHECK(merged.size() == 1);
  }

  // A merged occurrence is declined only when every copy was declined.
  {
    std::map<std::string, std::vector<CalendarEvent>> byAccount;
    CalendarEvent declinedCopy = event("Work", "Review", 60, 90);
    declinedCopy.declined = true;
    declinedCopy.location = "Room 1"; // richer, so it is the representative
    byAccount["work"] = {declinedCopy};
    byAccount["personal"] = {event("Personal", "Review", 60, 90)};
    const auto merged = calendar::mergeCalendarEvents(byAccount, true);
    TEST_CHECK(merged.size() == 1 && !merged.front().declined);

    byAccount["personal"].front().declined = true;
    const auto allDeclined = calendar::mergeCalendarEvents(byAccount, true);
    TEST_CHECK(allDeclined.size() == 1 && allDeclined.front().declined);
  }

  return 0;
}
