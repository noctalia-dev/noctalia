#include "calendar/calendar_types.h"
#include "ui/controls/calendar_view.h"

#include <chrono>
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

  CalendarEvent timedEvent(system_clock::time_point start, system_clock::time_point end) {
    return CalendarEvent{.start = start, .end = end, .allDay = false};
  }

  CalendarEvent allDayEvent(system_clock::time_point start, system_clock::time_point end) {
    return CalendarEvent{.start = start, .end = end, .allDay = true};
  }

} // namespace

int main() {
  bool ok = true;

  const auto now = utc(2026, 8, 22, 12);

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
             !eventPassed(allDayEvent(utc(2026, 8, 22), utc(2026, 8, 23)), now),
             "today's all-day event was passed at noon"
         )
        && ok;
    ok = expect(
             eventPassed(allDayEvent(utc(2026, 8, 21), utc(2026, 8, 22)), now),
             "yesterday's all-day event was not passed"
         )
        && ok;
  }

  // Feeds that omit DTEND leave end == start. An all-day event must still own its whole day.
  {
    const auto midnight = utc(2026, 8, 22);
    ok = expect(!eventPassed(allDayEvent(midnight, midnight), now), "all-day event without DTEND was passed") && ok;
    ok = expect(
             eventPassed(allDayEvent(utc(2026, 8, 20), utc(2026, 8, 20)), now),
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
