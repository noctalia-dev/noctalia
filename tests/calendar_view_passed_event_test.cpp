#include "calendar/calendar_types.h"
#include "ui/controls/calendar_view.h"

#include <chrono>
#include <ctime>
#include <print>

namespace {

  using namespace std::chrono;
  using calendar_view::eventPassed;

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "calendar_view_passed_event_test: {}", message);
    }
    return condition;
  }

  system_clock::time_point utc(int y, int m, int d, int h = 0) {
    return sys_days{year{y} / month{static_cast<unsigned>(m)} / day{static_cast<unsigned>(d)}} + hours(h);
  }

  system_clock::time_point localMidnight(int y, int m, int d) {
    std::tm value{};
    value.tm_year = y - 1900;
    value.tm_mon = m - 1;
    value.tm_mday = d;
    value.tm_isdst = -1;
    return system_clock::from_time_t(std::mktime(&value));
  }

  CalendarEvent timedEvent(system_clock::time_point start, system_clock::time_point end) {
    return CalendarEvent{.start = start, .end = end, .allDay = false};
  }

  CalendarEvent allDayEvent(system_clock::time_point start, system_clock::time_point end) {
    return CalendarEvent{.start = start, .end = end, .allDay = true};
  }

} // namespace

int main() {
  bool ok = true;

  const auto now = localMidnight(2026, 8, 22) + hours{12};

  {
    ok = expect(eventPassed(timedEvent(now - hours{2}, now - hours{1}), now), "finished event was not passed") && ok;
    ok = expect(!eventPassed(timedEvent(now + hours{1}, now + hours{2}), now), "upcoming event was passed") && ok;
  }

  {
    ok = expect(!eventPassed(timedEvent(now - hours{1}, now), now), "event ending exactly now was passed") && ok;
  }

  {
    const CalendarEvent event = timedEvent(utc(2026, 8, 20) + hours{9}, now + hours{3});
    ok = expect(!eventPassed(event, now), "running multi-day event was passed") && ok;
  }

  {
    ok = expect(
             !eventPassed(allDayEvent(localMidnight(2026, 8, 22), localMidnight(2026, 8, 23)), now),
             "today's all-day event was passed at noon"
         )
        && ok;
    ok = expect(
             eventPassed(allDayEvent(localMidnight(2026, 8, 21), localMidnight(2026, 8, 22)), now),
             "yesterday's all-day event was not passed"
         )
        && ok;
  }

  {
    const auto start = utc(2026, 3, 8);
    const auto end = start + hours{23};
    ok = expect(
             eventPassed(allDayEvent(start, end), end + hours{1}),
             "all-day event with a shorter civil-day end was not passed"
         )
        && ok;
  }

  // Feeds that omit DTEND leave end == start. An all-day event must still own its whole day.
  {
    const auto midnight = localMidnight(2026, 8, 22);
    ok = expect(!eventPassed(allDayEvent(midnight, midnight), now), "all-day event without DTEND was passed") && ok;
    ok = expect(
             eventPassed(allDayEvent(localMidnight(2026, 8, 20), localMidnight(2026, 8, 20)), now),
             "past all-day event without DTEND was not passed"
         )
        && ok;
  }

  {
    ok = expect(eventPassed(timedEvent(now - hours{1}, now - hours{1}), now), "zero-length event was not passed") && ok;
  }

  {
    ok = expect(!eventPassed(timedEvent(now + hours{2}, now - hours{2}), now), "inverted range was passed") && ok;
  }

  return ok ? 0 : 1;
}
