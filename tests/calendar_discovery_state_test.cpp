#include "calendar/calendar_discovery_state.h"
#include "tests/test_check.h"

#include <string>
#include <vector>

namespace {

  using Ids = std::vector<std::string>;

} // namespace

int main() {
  const std::vector<CalendarSource> discovered{
      {.id = "personal", .name = "Personal"},
      {.id = "work", .name = "Work"},
  };

  const std::vector<CalendarSource> parsed =
      calendar::parseCalendarSources(calendar::serializeCalendarSources(discovered));
  TEST_CHECK(parsed == discovered);

  TEST_CHECK(calendar::selectedCalendarSourceIds(discovered, {}) == Ids{"personal", "work"});
  TEST_CHECK(calendar::selectedCalendarSourceIds(discovered, {"work"}) == Ids{"work"});
  TEST_CHECK(calendar::selectedCalendarSourceIds(discovered, {"missing"}).empty());
  TEST_CHECK(calendar::setCalendarSourceChecked(discovered, {}, "work", false) == Ids{"personal"});
  TEST_CHECK(calendar::setCalendarSourceChecked(discovered, {"personal"}, "work", true).empty());
  TEST_CHECK(calendar::setCalendarSourceChecked(discovered, {"work"}, "work", false) == Ids{"work"});
  TEST_CHECK(calendar::setCalendarSourceChecked(discovered, {"work"}, "personal", true).empty());
  TEST_CHECK(calendar::parseCalendarSources("{}").empty());

  // Google lists every calendar, but an empty selection must keep syncing only those ticked in Google's sidebar.
  const std::vector<CalendarSource> google{
      {.id = "me@example.com", .name = "Personal", .defaultSelected = true},
      {.id = "fr#holiday@group.v.calendar.google.com", .name = "Holidays", .defaultSelected = false},
      {.id = "team@group.calendar.google.com", .name = "Team", .defaultSelected = true},
  };
  TEST_CHECK(calendar::parseCalendarSources(calendar::serializeCalendarSources(google)) == google);
  // A calendar without default_selected is selected by default.
  TEST_CHECK(
      calendar::parseCalendarSources(R"({"calendars":[{"id":"personal","name":"Personal"}]})").front().defaultSelected
  );
  TEST_CHECK(
      calendar::selectedCalendarSourceIds(google, {}) == Ids{"me@example.com", "team@group.calendar.google.com"}
  );
  TEST_CHECK(
      calendar::selectedCalendarSourceIds(google, {"fr#holiday@group.v.calendar.google.com"})
      == Ids{"fr#holiday@group.v.calendar.google.com"}
  );
  // Unticking in Noctalia starts from Google's ticks, not from every listed calendar.
  TEST_CHECK(
      calendar::setCalendarSourceChecked(google, {}, "team@group.calendar.google.com", false) == Ids{"me@example.com"}
  );
  // Ticking everything is an explicit choice, not a return to following Google's sidebar.
  TEST_CHECK(
      calendar::setCalendarSourceChecked(google, {}, "fr#holiday@group.v.calendar.google.com", true)
      == Ids{"me@example.com", "team@group.calendar.google.com", "fr#holiday@group.v.calendar.google.com"}
  );
  TEST_CHECK(
      calendar::setCalendarSourceChecked(google, {"me@example.com"}, "team@group.calendar.google.com", true).empty()
  );
  TEST_CHECK(calendar::parseCalendarSources("not json").empty());

  return 0;
}
