#include "calendar/calendar_discovery_state.h"

#include <cstdio>
#include <print>
#include <string>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "calendar_discovery_state_test: {}", message);
      return false;
    }
    return true;
  }

  bool expectIds(const std::vector<std::string>& ids, const std::vector<std::string>& expected) {
    if (ids.size() != expected.size()) {
      std::println(stderr, "calendar_discovery_state_test: expected {} ids, got {}", expected.size(), ids.size());
      return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
      if (ids[i] != expected[i]) {
        std::println(stderr, "calendar_discovery_state_test: id {} expected {}, got {}", i, expected[i], ids[i]);
        return false;
      }
    }
    return true;
  }

} // namespace

int main() {
  const std::vector<CalendarSource> discovered{
      {.id = "personal", .name = "Personal"},
      {.id = "work", .name = "Work"},
  };

  bool ok = true;
  const std::vector<CalendarSource> parsed =
      calendar::parseCalendarSources(calendar::serializeCalendarSources(discovered));
  ok = expect(parsed == discovered, "serialized discovery metadata round-trips") && ok;

  ok = expectIds(calendar::selectedCalendarSourceIds(discovered, {}), {"personal", "work"}) && ok;
  ok = expectIds(calendar::selectedCalendarSourceIds(discovered, {"work"}), {"work"}) && ok;
  ok = expectIds(calendar::selectedCalendarSourceIds(discovered, {"missing"}), {}) && ok;
  ok = expectIds(calendar::setCalendarSourceChecked(discovered, {}, "work", false), {"personal"}) && ok;
  ok = expectIds(calendar::setCalendarSourceChecked(discovered, {"personal"}, "work", true), {}) && ok;
  ok = expectIds(calendar::setCalendarSourceChecked(discovered, {"work"}, "work", false), {"work"}) && ok;
  ok = expectIds(calendar::setCalendarSourceChecked(discovered, {"work"}, "personal", true), {}) && ok;
  ok = expect(calendar::parseCalendarSources("{}").empty(), "missing calendar array parses as empty") && ok;

  // Google lists every calendar, but an empty selection must keep syncing only those ticked in Google's sidebar.
  const std::vector<CalendarSource> google{
      {.id = "me@example.com", .name = "Personal", .defaultSelected = true},
      {.id = "fr#holiday@group.v.calendar.google.com", .name = "Holidays", .defaultSelected = false},
      {.id = "team@group.calendar.google.com", .name = "Team", .defaultSelected = true},
  };
  ok = expect(
           calendar::parseCalendarSources(calendar::serializeCalendarSources(google)) == google,
           "Google sidebar ticks round-trip through discovery state"
       )
      && ok;
  ok = expect(
           calendar::parseCalendarSources(R"({"calendars":[{"id":"personal","name":"Personal"}]})")
               .front()
               .defaultSelected,
           "a calendar without default_selected is selected by default"
       )
      && ok;
  ok = expectIds(calendar::selectedCalendarSourceIds(google, {}), {"me@example.com", "team@group.calendar.google.com"})
      && ok;
  ok = expectIds(
           calendar::selectedCalendarSourceIds(google, {"fr#holiday@group.v.calendar.google.com"}),
           {"fr#holiday@group.v.calendar.google.com"}
       )
      && ok;
  // Unticking in Noctalia starts from Google's ticks, not from every listed calendar.
  ok = expectIds(
           calendar::setCalendarSourceChecked(google, {}, "team@group.calendar.google.com", false), {"me@example.com"}
       )
      && ok;
  // Ticking everything is an explicit choice, not a return to following Google's sidebar.
  ok = expectIds(
           calendar::setCalendarSourceChecked(google, {}, "fr#holiday@group.v.calendar.google.com", true),
           {"me@example.com", "team@group.calendar.google.com", "fr#holiday@group.v.calendar.google.com"}
       )
      && ok;
  ok = expectIds(
           calendar::setCalendarSourceChecked(google, {"me@example.com"}, "team@group.calendar.google.com", true), {}
       )
      && ok;
  ok = expect(calendar::parseCalendarSources("not json").empty(), "invalid discovery JSON parses as empty") && ok;

  return ok ? 0 : 1;
}
