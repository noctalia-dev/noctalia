#include "calendar/calendar_discovery_state.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "calendar_discovery_state_test: %s\n", message);
      return false;
    }
    return true;
  }

  bool expectIds(const std::vector<std::string>& ids, const std::vector<std::string>& expected) {
    if (ids.size() != expected.size()) {
      std::fprintf(stderr, "calendar_discovery_state_test: expected %zu ids, got %zu\n", expected.size(), ids.size());
      return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
      if (ids[i] != expected[i]) {
        std::fprintf(
            stderr, "calendar_discovery_state_test: id %zu expected %s, got %s\n", i, expected[i].c_str(),
            ids[i].c_str()
        );
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
  ok = expect(calendar::parseCalendarSources("not json").empty(), "invalid discovery JSON parses as empty") && ok;

  return ok ? 0 : 1;
}
