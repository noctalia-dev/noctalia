#include "calendar/calendar_types.h"
#include "shell/calendar/calendar_view.h"
#include "tests/test_check.h"

#include <chrono>
#include <ctime>

namespace {

  using namespace std::chrono;
  using calendar_view::countHiddenPastEvents;
  using calendar_view::eventHiddenAsPast;
  using calendar_view::eventPassed;

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
  const auto now = localMidnight(2026, 8, 22) + hours{12};

  {
    TEST_CHECK(eventPassed(timedEvent(now - hours{2}, now - hours{1}), now));
    TEST_CHECK(!eventPassed(timedEvent(now + hours{1}, now + hours{2}), now));
  }

  {
    TEST_CHECK(!eventPassed(timedEvent(now - hours{1}, now), now));
  }

  {
    const CalendarEvent event = timedEvent(utc(2026, 8, 20) + hours{9}, now + hours{3});
    TEST_CHECK(!eventPassed(event, now));
  }

  {
    TEST_CHECK(!eventPassed(allDayEvent(localMidnight(2026, 8, 22), localMidnight(2026, 8, 23)), now));
    TEST_CHECK(eventPassed(allDayEvent(localMidnight(2026, 8, 21), localMidnight(2026, 8, 22)), now));
  }

  {
    const auto start = utc(2026, 3, 8);
    const auto end = start + hours{23};
    // All-day event with a shorter civil-day end.
    TEST_CHECK(eventPassed(allDayEvent(start, end), end + hours{1}));
  }

  // Feeds that omit DTEND leave end == start. An all-day event must still own its whole day.
  {
    const auto midnight = localMidnight(2026, 8, 22);
    TEST_CHECK(!eventPassed(allDayEvent(midnight, midnight), now));
    TEST_CHECK(eventPassed(allDayEvent(localMidnight(2026, 8, 20), localMidnight(2026, 8, 20)), now));
  }

  {
    TEST_CHECK(eventPassed(timedEvent(now - hours{1}, now - hours{1}), now));
  }

  {
    TEST_CHECK(!eventPassed(timedEvent(now + hours{2}, now - hours{2}), now));
  }

  {
    const auto done = timedEvent(now - hours{2}, now - minutes{45});
    const auto recent = timedEvent(now - hours{1}, now - minutes{10});
    const auto upcoming = timedEvent(now + hours{1}, now + hours{2});
    TEST_CHECK(!eventHiddenAsPast(done, now, false, 30));
    TEST_CHECK(!eventHiddenAsPast(recent, now, false, 0));
    TEST_CHECK(eventHiddenAsPast(done, now, true, 30));
    TEST_CHECK(!eventHiddenAsPast(recent, now, true, 30));
    TEST_CHECK(eventHiddenAsPast(recent, now, true, 0));
    TEST_CHECK(!eventHiddenAsPast(upcoming, now, true, 0));
  }

  {
    CalendarSnapshot snapshot;
    snapshot.events = {
        timedEvent(now - hours{3}, now - hours{2}),
        timedEvent(now - hours{1}, now - minutes{10}),
        timedEvent(now + hours{1}, now + hours{2}),
    };
    const calendar_view::Date today{.year = 2026, .month = 7, .day = 22};
    const calendar_view::Date other{.year = 2026, .month = 7, .day = 21};
    TEST_CHECK(countHiddenPastEvents(snapshot, today, now, true, 30) == 1);
    TEST_CHECK(countHiddenPastEvents(snapshot, today, now, true, 0) == 2);
    TEST_CHECK(countHiddenPastEvents(snapshot, today, now, false, 0) == 0);
    TEST_CHECK(countHiddenPastEvents(snapshot, other, now, true, 0) == 0);
  }

  return 0;
}
