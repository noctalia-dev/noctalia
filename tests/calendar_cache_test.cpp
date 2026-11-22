#include "calendar/calendar_cache.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

int main() {
  using namespace std::chrono;

  CalendarEvent unspecified;
  unspecified.id = "a";
  unspecified.title = "Standup";
  unspecified.calendarName = "Work";
  unspecified.colorHex = "#3367d6";
  unspecified.sourceId = "work";
  unspecified.location = "https://meet.example.com/x";
  unspecified.url = "https://meet.example.com/x";
  unspecified.webUrl = "https://calendar.example.com/a";
  unspecified.start = system_clock::time_point{seconds{1'700'000'000}};
  unspecified.end = unspecified.start + hours{1};
  unspecified.declined = true;

  CalendarEvent noReminder = unspecified;
  noReminder.id = "b";
  noReminder.allDay = true;
  noReminder.declined = false;
  noReminder.reminderLeadSeconds = std::vector<std::int32_t>{};

  CalendarEvent withReminders = unspecified;
  withReminders.id = "c";
  withReminders.reminderLeadSeconds = std::vector<std::int32_t>{0, 600};

  const std::map<std::string, std::vector<CalendarEvent>> events{
      {"google", {unspecified, noReminder}},
      {"caldav", {withReminders}},
  };

  const auto parsed = calendar::cache::parseEvents(calendar::cache::serializeEvents(events));
  TEST_CHECK(parsed.size() == 2);
  TEST_CHECK(parsed.at("google").size() == 2);
  TEST_CHECK(parsed.at("caldav").size() == 1);

  const CalendarEvent& a = parsed.at("google")[0];
  TEST_CHECK(a.id == "a");
  TEST_CHECK(a.title == "Standup");
  TEST_CHECK(a.calendarName == "Work");
  TEST_CHECK(a.colorHex == "#3367d6");
  TEST_CHECK(a.sourceId == "work");
  TEST_CHECK(a.location == unspecified.location);
  TEST_CHECK(a.url == unspecified.url);
  TEST_CHECK(a.webUrl == unspecified.webUrl);
  TEST_CHECK(a.start == unspecified.start);
  TEST_CHECK(a.end == unspecified.end);
  TEST_CHECK(!a.allDay);
  TEST_CHECK(a.declined);
  TEST_CHECK(!a.reminderLeadSeconds.has_value());

  const CalendarEvent& b = parsed.at("google")[1];
  TEST_CHECK(b.id == "b");
  TEST_CHECK(b.allDay);
  TEST_CHECK(b.reminderLeadSeconds.has_value() && b.reminderLeadSeconds->empty());

  const CalendarEvent& c = parsed.at("caldav")[0];
  TEST_CHECK(c.reminderLeadSeconds == withReminders.reminderLeadSeconds);

  return 0;
}
