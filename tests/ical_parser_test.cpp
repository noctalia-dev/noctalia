#include "calendar/calendar_reminders.h"
#include "calendar/ical_parser.h"
#include "render/core/color.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

  using namespace std::chrono;
  using calendar::ICalParseControl;
  using calendar::ICalParseResult;
  using calendar::ICalParseStatus;

  system_clock::time_point utc(int y, int mo, int d, int h = 0) {
    return sys_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}} + hours{h};
  }

  ICalParseResult parseEvents(const std::string& ics, system_clock::time_point start, system_clock::time_point end) {
    ICalParseControl control;
    return calendar::parseICalEvents(ics, start, end, control);
  }

  // Local midnight of a civil date in the system zone, matching how the parser anchors all-day
  // occurrences. current_zone() reads /etc/localtime and ignores TZ, so all-day expectations must be
  // computed from it rather than hardcoded to a single zone.
  system_clock::time_point localMidnight(int y, int mo, int d) {
    const local_days ld{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}};
    try {
      return time_point_cast<system_clock::duration>(time_point_cast<seconds>(current_zone()->to_sys(ld)));
    } catch (...) {
      return sys_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}};
    }
  }

  system_clock::time_point localTime(int y, int mo, int d, int h, int mi = 0) {
    const local_seconds lt =
        local_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}} + hours{h} + minutes{mi};
    try {
      return time_point_cast<system_clock::duration>(time_point_cast<seconds>(current_zone()->to_sys(lt)));
    } catch (...) {
      const sys_days civilDay{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}};
      return civilDay + hours{h} + minutes{mi};
    }
  }

  // True when the parsed occurrences' start instants exactly match `expected` (order-independent).
  bool parsesToStarts(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end,
      std::vector<system_clock::time_point> expected
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    if (result.status != ICalParseStatus::Complete) {
      return false;
    }
    std::vector<system_clock::time_point> actual;
    actual.reserve(result.events.size());
    for (const auto& ev : result.events) {
      actual.push_back(ev.start);
    }
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    return actual == expected;
  }

  // A VEVENT starting Mon 2024-01-01 09:00 UTC (1h long), with the given extra property lines appended.
  std::string wrap(const std::string& props) {
    return "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:s\r\nDTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\n"
        + props
        + "END:VEVENT\r\n";
  }

  bool parsesToCount(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end, std::size_t expected
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    return result.status == ICalParseStatus::Complete && result.events.size() == expected;
  }

  bool parsesToRanges(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end,
      std::vector<std::pair<system_clock::time_point, system_clock::time_point>> expected
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    if (result.status != ICalParseStatus::Complete) {
      return false;
    }
    std::vector<std::pair<system_clock::time_point, system_clock::time_point>> actual;
    actual.reserve(result.events.size());
    for (const auto& ev : result.events) {
      actual.emplace_back(ev.start, ev.end);
    }
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    return actual == expected;
  }

  bool parsesToOneEventText(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end, const std::string& title,
      const std::string& location
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    const std::vector<CalendarEvent>& events = result.events;
    return result.status == ICalParseStatus::Complete
        && events.size() == 1
        && events.front().title == title
        && events.front().location == location;
  }

  bool parsesToOneEventUrl(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end, const std::string& url
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    return result.status == ICalParseStatus::Complete && result.events.size() == 1 && result.events.front().url == url;
  }

} // namespace

int main() {
  const auto start = utc(2024, 1, 1);
  const auto end = utc(2024, 2, 1);

  // No RRULE: exactly one instance passes through.
  TEST_CHECK(parsesToCount(wrap(""), start, end, 1));

  // Standard VCALENDAR wrappers and multiple VEVENT children are accepted.
  {
    const std::string ics =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
        "BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:b\r\nDTSTART:20240102T090000Z\r\nDTEND:20240102T100000Z\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    // vcalendar with multiple events
    TEST_CHECK(parsesToCount(ics, start, end, 2));
  }

  // Libical handles folded content lines and RFC text escaping.
  {
    const std::string ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:text\r\n"
                            "SUMMARY:Planning\\n\r\n Session\r\n"
                            "LOCATION:Main\\, Room\r\n"
                            "DTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    // escaped text values
    TEST_CHECK(parsesToOneEventText(ics, start, end, "Planning\nSession", "Main, Room"));
  }

  // A meeting link in LOCATION becomes the event's clickable url, and wins over the URL property.
  {
    const std::string ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:meet\r\nSUMMARY:Daily Sync\r\n"
                            "LOCATION:https://meet.google.com/abc-defg-hij\r\n"
                            "URL:https://calendar.example/event/1\r\n"
                            "DTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    // location link wins over url
    TEST_CHECK(parsesToOneEventUrl(ics, start, end, "https://meet.google.com/abc-defg-hij"));
  }

  // With no link in LOCATION the URL property is used.
  {
    const std::string ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:url\r\nSUMMARY:Review\r\n"
                            "LOCATION:Meeting Room 3\r\n"
                            "URL:https://calendar.example/event/2\r\n"
                            "DTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    // url property fallback
    TEST_CHECK(parsesToOneEventUrl(ics, start, end, "https://calendar.example/event/2"));
  }

  // Outlook places Teams links in DESCRIPTION, often as an HTML anchor, while URL points to an event page.
  {
    const std::string ics =
        "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:teams\r\nSUMMARY:Standup\r\n"
        "LOCATION:Microsoft Teams Meeting\r\n"
        "DESCRIPTION:Join now: <a "
        "href=\"https://teams.microsoft.com/l/meetup-join/19%3ameeting_abc%40thread.v2/0?context=%7b%7d\">Join</a>\r\n"
        "URL:https://outlook.office.com/calendar/item/3\r\n"
        "DTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    // Teams description link wins over url property
    TEST_CHECK(parsesToOneEventUrl(
        ics, start, end, "https://teams.microsoft.com/l/meetup-join/19%3ameeting_abc%40thread.v2/0?context=%7b%7d"
    ));
  }

  // An event with neither carries no link.
  {
    const std::string ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:plain\r\nSUMMARY:Dentist\r\n"
                            "LOCATION:Kyiv\r\n"
                            "DTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    // no link
    TEST_CHECK(parsesToOneEventUrl(ics, start, end, ""));
  }

  // Malformed/incomplete VEVENTs are skipped instead of producing epoch-placeholder events.
  {
    const std::string ics =
        "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:bad\r\nSUMMARY:missing start\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    // event without dtstart is skipped
    TEST_CHECK(parsesToCount(ics, start, end, 0));
  }

  // The Unix epoch is a valid event instant and must not be confused with an invalid timestamp.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:epoch\r\nSUMMARY:s\r\n"
                            "DTSTART:19700101T000000Z\r\nDTEND:19700101T010000Z\r\n"
                            "END:VEVENT\r\n";
    // unix epoch event
    TEST_CHECK(parsesToRanges(
        ics, utc(1969, 12, 31), utc(1970, 1, 2), {{system_clock::time_point{}, system_clock::time_point{} + hours{1}}}
    ));
  }

  // DAILY COUNT=3 -> 3 instances.
  TEST_CHECK(parsesToCount(wrap("RRULE:FREQ=DAILY;COUNT=3\r\n"), start, end, 3));

  // RDATE contributes recurrence instances even when the component has no RRULE.
  TEST_CHECK(parsesToStarts(wrap("RDATE:20240102T090000Z\r\n"), start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 2, 9)}));

  // RRULE and RDATE are a set: an RDATE equal to an RRULE occurrence must not create a duplicate.
  TEST_CHECK(parsesToStarts(
      wrap("RRULE:FREQ=DAILY;COUNT=2\r\nRDATE:20240102T090000Z\r\n"), start, end,
      {utc(2024, 1, 1, 9), utc(2024, 1, 2, 9)}
  ));

  // An explicit RDATE period overrides the master's duration when it shares an RRULE occurrence.
  TEST_CHECK(parsesToRanges(
      wrap(
          "RRULE:FREQ=DAILY;COUNT=2\r\n"
          "RDATE;VALUE=PERIOD:20240102T090000Z/20240102T120000Z\r\n"
      ),
      start, end, {{utc(2024, 1, 1, 9), utc(2024, 1, 1, 10)}, {utc(2024, 1, 2, 9), utc(2024, 1, 2, 12)}}
  ));

  // An explicit RDATE period may express its end as a duration instead of an absolute timestamp.
  TEST_CHECK(parsesToRanges(
      wrap("RDATE;VALUE=PERIOD:20240102T090000Z/PT3H\r\n"), start, end,
      {{utc(2024, 1, 1, 9), utc(2024, 1, 1, 10)}, {utc(2024, 1, 2, 9), utc(2024, 1, 2, 12)}}
  ));

  // WEEKLY BYDAY=MO,WE, window 2024-01-01..2024-01-15 (excl): Mon 1, Wed 3, Mon 8, Wed 10 = 4.
  TEST_CHECK(parsesToCount(wrap("RRULE:FREQ=WEEKLY;BYDAY=MO,WE\r\n"), start, utc(2024, 1, 15), 4));

  // WEEKLY INTERVAL=2 (no BYDAY): every other Monday from Jan 1 to Feb 1: Jan 1, 15, 29 = 3.
  TEST_CHECK(parsesToCount(wrap("RRULE:FREQ=WEEKLY;INTERVAL=2\r\n"), start, end, 3));

  // MONTHLY unbounded over one year: Jan..Dec 2024 = 12.
  TEST_CHECK(parsesToCount(wrap("RRULE:FREQ=MONTHLY\r\n"), start, utc(2025, 1, 1), 12));

  // A timed DTSTART with TZID must be converted exactly once. Kyiv is UTC+3 in July, so 11:00 local
  // is 08:00Z; treating the zoned wall time as UTC would display several hours late.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:kyiv-single\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=Europe/Kyiv:20240705T110000\r\n"
                            "DTEND;TZID=Europe/Kyiv:20240705T120000\r\n"
                            "END:VEVENT\r\n";
    // timed Kyiv event converts timezone once
    TEST_CHECK(parsesToRanges(ics, utc(2024, 7, 1), utc(2024, 8, 1), {{utc(2024, 7, 5, 8), utc(2024, 7, 5, 9)}}));
  }

  // Floating DATE-TIME values have no Z suffix and no TZID. They are local wall times, not UTC
  // instants, so 11:00 must be anchored through the user's current zone.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-single\r\nSUMMARY:s\r\n"
                            "DTSTART:20240705T110000\r\n"
                            "DTEND:20240705T120000\r\n"
                            "END:VEVENT\r\n";
    // floating timed event uses local wall time
    TEST_CHECK(
        parsesToRanges(ics, utc(2024, 7, 1), utc(2024, 8, 1), {{localTime(2024, 7, 5, 11), localTime(2024, 7, 5, 12)}})
    );
  }

  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-recurring\r\nSUMMARY:s\r\n"
                            "DTSTART:20240705T110000\r\n"
                            "DTEND:20240705T120000\r\nRRULE:FREQ=DAILY;COUNT=2\r\n"
                            "END:VEVENT\r\n";
    // floating timed recurrence uses local wall time
    TEST_CHECK(parsesToRanges(
        ics, utc(2024, 7, 1), utc(2024, 8, 1),
        {{localTime(2024, 7, 5, 11), localTime(2024, 7, 5, 12)}, {localTime(2024, 7, 6, 11), localTime(2024, 7, 6, 12)}}
    ));
  }

  // Libical compares floating recurrence values to the supplied bounds as wall times. A 23:00
  // floating occurrence in Europe/Kyiv is 20:00Z, so UTC-field bounds ending at 21:30 incorrectly
  // exclude it before the callback can apply the real instant-based window filter.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-window-boundary\r\nSUMMARY:s\r\n"
                            "DTSTART:20240705T230000\r\n"
                            "DTEND:20240706T000000\r\nRRULE:FREQ=DAILY;COUNT=2\r\n"
                            "END:VEVENT\r\n";
    const system_clock::time_point occurrence = localTime(2024, 7, 5, 23);
    // floating recurrence within UTC window boundary
    TEST_CHECK(parsesToStarts(ics, occurrence - minutes{30}, occurrence + minutes{90}, {occurrence}));
  }

  // Floating recurrences remain local wall times across the system zone's DST boundary. In
  // Europe/Kyiv this crosses the Mar 31 2024 spring-forward transition; in zones without that
  // transition the expectation still verifies local anchoring rather than UTC anchoring.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-dst\r\nSUMMARY:s\r\n"
                            "DTSTART:20240329T110000\r\n"
                            "DTEND:20240329T120000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                            "END:VEVENT\r\n";
    // floating timed recurrence preserves local wall time across dst
    TEST_CHECK(parsesToRanges(
        ics, utc(2024, 3, 28), utc(2024, 4, 4),
        {{localTime(2024, 3, 29, 11), localTime(2024, 3, 29, 12)},
         {localTime(2024, 3, 30, 11), localTime(2024, 3, 30, 12)},
         {localTime(2024, 3, 31, 11), localTime(2024, 3, 31, 12)},
         {localTime(2024, 4, 1, 11), localTime(2024, 4, 1, 12)},
         {localTime(2024, 4, 2, 11), localTime(2024, 4, 2, 12)}}
    ));
  }

  // Timed monthly recurrences with TZID must preserve the local wall time across DST. Kyiv is UTC+2
  // in February and UTC+3 in July, so an 11:00 February event should recur at 08:00Z in July.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:kyiv-monthly\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=Europe/Kyiv:20240205T110000\r\n"
                            "DTEND;TZID=Europe/Kyiv:20240205T120000\r\nRRULE:FREQ=MONTHLY;COUNT=6\r\n"
                            "END:VEVENT\r\n";
    // monthly timed recurrence preserves Kyiv wall time across dst
    TEST_CHECK(parsesToRanges(ics, utc(2024, 7, 1), utc(2024, 8, 1), {{utc(2024, 7, 5, 8), utc(2024, 7, 5, 9)}}));
  }

  // Daily TZID recurrences preserve local wall time across DST. New York switches from UTC-5 to
  // UTC-4 on Mar 10 2024, so the UTC instant moves from 14:00Z to 13:00Z while local time stays 09:00.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ny-daily-dst\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=America/New_York:20240308T090000\r\n"
                            "DTEND;TZID=America/New_York:20240308T100000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                            "END:VEVENT\r\n";
    // daily timed recurrence preserves New York wall time across dst
    TEST_CHECK(parsesToRanges(
        ics, utc(2024, 3, 8), utc(2024, 3, 14),
        {{utc(2024, 3, 8, 14), utc(2024, 3, 8, 15)},
         {utc(2024, 3, 9, 14), utc(2024, 3, 9, 15)},
         {utc(2024, 3, 10, 13), utc(2024, 3, 10, 14)},
         {utc(2024, 3, 11, 13), utc(2024, 3, 11, 14)},
         {utc(2024, 3, 12, 13), utc(2024, 3, 12, 14)}}
    ));
  }

  // Monthly recurrences on a month-end date skip invalid civil dates instead of approximating with a
  // fixed day count. Jan 31 + MONTHLY yields Jan 31, Mar 31, May 31, Jul 31... because February and
  // April have no 31st.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:month-end\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=Europe/Kyiv:20240131T110000\r\n"
                            "DTEND;TZID=Europe/Kyiv:20240131T120000\r\nRRULE:FREQ=MONTHLY;COUNT=4\r\n"
                            "END:VEVENT\r\n";
    // monthly timed recurrence skips invalid month-end dates
    TEST_CHECK(parsesToRanges(
        ics, utc(2024, 1, 1), utc(2024, 6, 15),
        {{utc(2024, 1, 31, 9), utc(2024, 1, 31, 10)},
         {utc(2024, 3, 31, 8), utc(2024, 3, 31, 9)},
         {utc(2024, 5, 31, 8), utc(2024, 5, 31, 9)}}
    ));
  }

  // UNTIL clips: DAILY until Jan 3 -> Jan 1, 2, 3 = 3.
  TEST_CHECK(parsesToCount(wrap("RRULE:FREQ=DAILY;UNTIL=20240103T090000Z\r\n"), start, end, 3));

  // UTC UNTIL clips a TZID recurrence by instant. Mar 10 09:00 New York is 13:00Z after the DST jump,
  // so an UNTIL at 13:00Z includes Mar 8, Mar 9, and Mar 10.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ny-until\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=America/New_York:20240308T090000\r\n"
                            "DTEND;TZID=America/New_York:20240308T100000\r\n"
                            "RRULE:FREQ=DAILY;UNTIL=20240310T130000Z\r\n"
                            "END:VEVENT\r\n";
    // utc until clips timezone recurrence by instant
    TEST_CHECK(parsesToStarts(
        ics, utc(2024, 3, 8), utc(2024, 3, 14), {utc(2024, 3, 8, 14), utc(2024, 3, 9, 14), utc(2024, 3, 10, 13)}
    ));
  }

  // EXDATE drops one occurrence: DAILY COUNT=5 minus Jan 3 = 4.
  TEST_CHECK(parsesToCount(wrap("RRULE:FREQ=DAILY;COUNT=5\r\nEXDATE:20240103T090000Z\r\n"), start, end, 4));

  // Floating EXDATE values match floating recurrence instances after local wall-time anchoring.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-exdate\r\nSUMMARY:s\r\n"
                            "DTSTART:20240705T110000\r\n"
                            "DTEND:20240705T120000\r\nRRULE:FREQ=DAILY;COUNT=3\r\n"
                            "EXDATE:20240706T110000\r\nEND:VEVENT\r\n";
    // floating exdate excludes local wall-time occurrence
    TEST_CHECK(
        parsesToStarts(ics, utc(2024, 7, 1), utc(2024, 8, 1), {localTime(2024, 7, 5, 11), localTime(2024, 7, 7, 11)})
    );
  }

  // Multiple EXDATE values on one property are common in exported calendars and must exclude each
  // listed occurrence.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:multi-exdate\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=Europe/Kyiv:20240705T110000\r\n"
                            "DTEND;TZID=Europe/Kyiv:20240705T120000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                            "EXDATE;TZID=Europe/Kyiv:20240706T110000,20240708T110000\r\nEND:VEVENT\r\n";
    // multiple exdate values exclude listed occurrences
    TEST_CHECK(parsesToStarts(
        ics, utc(2024, 7, 1), utc(2024, 8, 1), {utc(2024, 7, 5, 8), utc(2024, 7, 7, 8), utc(2024, 7, 9, 8)}
    ));
  }

  // Window clips leading occurrences but COUNT still counts them: DAILY COUNT=10 from Dec 30 2023,
  // window opens Jan 1 -> Dec 30/31 not shown, Jan 1..8 shown = 8.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:y\r\nDTSTART:20231230T090000Z\r\nDTEND:20231230T100000Z\r\n"
                            "RRULE:FREQ=DAILY;COUNT=10\r\nEND:VEVENT\r\n";
    // count spans window start
    TEST_CHECK(parsesToCount(ics, start, end, 8));
  }

  // Unbounded daily series starting far before the window (2005) must still fill the whole Jan 2024
  // month window: 31 days. Guards against the iteration cap truncating an old series to nothing.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:z\r\nDTSTART:20050101T090000Z\r\nDTEND:20050101T100000Z\r\n"
                            "RRULE:FREQ=DAILY\r\nEND:VEVENT\r\n";
    // old unbounded daily reaches window
    TEST_CHECK(parsesToCount(ics, start, end, 31));
  }

  // An explicit COUNT must not disable the skip-ahead: a bounded daily series whose DTSTART is many
  // years (>4000 days) before the window still has to reach it. COUNT=6000 from 2013 spans past 2024,
  // so the whole Jan window shows = 31 days. Guards against COUNT truncating an old series to nothing.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:cnt\r\nDTSTART:20130101T090000Z\r\nDTEND:20130101T100000Z\r\n"
                            "RRULE:FREQ=DAILY;COUNT=6000\r\nEND:VEVENT\r\n";
    // old daily with count reaches window
    TEST_CHECK(parsesToCount(ics, start, end, 31));
  }

  // All-day (VALUE=DATE) MONTHLY on the 1st, over a one-year window, is exactly 12 - one per month.
  // The old code derived the day-of-month from floor<days> of the UTC instant, which for a zone east
  // of UTC rolls back to the previous civil day (the 31st of Dec), so months without a 31st were
  // dropped. Correct behavior is zone-independent. Window is padded ±half-month so each local-midnight
  // occurrence lands inside regardless of the running zone's UTC offset.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ad\r\nSUMMARY:s\r\nDTSTART;VALUE=DATE:20240101\r\n"
                            "DTEND;VALUE=DATE:20240102\r\nRRULE:FREQ=MONTHLY\r\nEND:VEVENT\r\n";
    // all-day monthly civil date
    TEST_CHECK(parsesToCount(ics, utc(2023, 12, 15), utc(2024, 12, 15), 12));
  }

  // All-day recurrences are also local-valued. In Europe/Kyiv, July 6 local midnight is July 5
  // 21:00Z and must be expanded when a UTC window covers that instant.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:all-day-window-boundary\r\nSUMMARY:s\r\n"
                            "DTSTART;VALUE=DATE:20240706\r\n"
                            "DTEND;VALUE=DATE:20240707\r\nRRULE:FREQ=DAILY;COUNT=2\r\n"
                            "END:VEVENT\r\n";
    const system_clock::time_point occurrence = localMidnight(2024, 7, 6);
    // all-day recurrence within UTC window boundary
    TEST_CHECK(parsesToStarts(ics, occurrence - hours{1}, occurrence + hours{1}, {occurrence}));
  }

  // Multi-day all-day recurrences must preserve their civil-day span, not a fixed UTC duration.
  // The May occurrence must span its own local midnights (May 30 -> Jun 1), computed against the
  // running zone. In a zone whose DST starts on Mar 31 (e.g. Europe/Kyiv), the Mar 30->Apr 1 master
  // is 47 UTC hours, so applying that fixed duration would end the May instance an hour early; the
  // civil-day span keeps it correct. Expectations track current_zone() so this holds in any zone.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:dst\r\nSUMMARY:s\r\nDTSTART;VALUE=DATE:20240330\r\n"
                            "DTEND;VALUE=DATE:20240401\r\nRRULE:FREQ=MONTHLY;COUNT=3\r\nEND:VEVENT\r\n";
    // all-day monthly preserves civil end across dst
    TEST_CHECK(parsesToRanges(
        ics, utc(2024, 5, 1), utc(2024, 6, 15), {{localMidnight(2024, 5, 30), localMidnight(2024, 6, 1)}}
    ));
  }

  // EXDATE must still exclude across a DST boundary. Occurrences hold a constant UTC instant (drifting
  // ~1h vs local wall time across DST), while the server's EXDATE carries the true local wall time - a
  // different instant. TZID makes the drift zone-independent: New York DST starts Mar 10 2024, so a
  // daily noon event with an EXDATE on Mar 15 (post-DST) must drop exactly that day: 20 - 1 = 19.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:dstx\r\nDTSTART;TZID=America/New_York:20240301T120000\r\n"
                            "DTEND;TZID=America/New_York:20240301T130000\r\nRRULE:FREQ=DAILY;COUNT=20\r\n"
                            "EXDATE;TZID=America/New_York:20240315T120000\r\nEND:VEVENT\r\n";
    // exdate excludes across dst
    TEST_CHECK(parsesToCount(ics, utc(2024, 3, 1), utc(2024, 4, 1), 19));
  }

  // Daily timed recurrences must not crash when an occurrence lands in a spring-forward gap. New York
  // jumps from 01:59 to 03:00 on Mar 10 2024, so 02:30 does not exist that day.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ny-gap\r\nDTSTART;TZID=America/New_York:20240308T023000\r\n"
                            "DTEND;TZID=America/New_York:20240308T033000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                            "END:VEVENT\r\n";
    // daily recurrence survives dst gap
    TEST_CHECK(parsesToCount(ics, utc(2024, 3, 8), utc(2024, 3, 14), 5));
  }

  // Fall-back ambiguous local times must produce a deterministic event instead of a duplicate or a
  // dropped occurrence. New York 2024-11-03 01:30 occurs twice when clocks fall back.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ny-fold\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=America/New_York:20241103T013000\r\n"
                            "DTEND;TZID=America/New_York:20241103T023000\r\n"
                            "END:VEVENT\r\n";
    // ambiguous fall-back time parses once
    TEST_CHECK(parsesToCount(ics, utc(2024, 11, 3), utc(2024, 11, 4), 1));
  }

  // RECURRENCE-ID override: a modified instance replaces the master's occurrence at that instant,
  // it does not add a duplicate. Master DAILY COUNT=3 (Jan 1/2/3 @09:00); the override moves Jan 3
  // to 14:00. Expect Jan 1 @09, Jan 2 @09, Jan 3 @14 - not four events, and no leftover Jan 3 @09.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:x\r\nDTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\n"
                            "RRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:x\r\nRECURRENCE-ID:20240103T090000Z\r\n"
                            "DTSTART:20240103T140000Z\r\nDTEND:20240103T150000Z\r\nEND:VEVENT\r\n";
    // recurrence-id override replaces occurrence
    TEST_CHECK(parsesToStarts(ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 2, 9), utc(2024, 1, 3, 14)}));
  }

  // Floating RECURRENCE-ID values replace the matching local wall-time occurrence, not the same
  // numeric UTC wall fields.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-override\r\nSUMMARY:s\r\n"
                            "DTSTART:20240705T110000\r\nDTEND:20240705T120000\r\n"
                            "RRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:floating-override\r\nRECURRENCE-ID:20240706T110000\r\n"
                            "DTSTART:20240706T150000\r\nDTEND:20240706T160000\r\nEND:VEVENT\r\n";
    // floating recurrence-id override replaces local wall-time occurrence
    TEST_CHECK(parsesToStarts(
        ics, utc(2024, 7, 1), utc(2024, 8, 1),
        {localTime(2024, 7, 5, 11), localTime(2024, 7, 6, 15), localTime(2024, 7, 7, 11)}
    ));
  }

  // A cancelled override removes the matching master occurrence and must not be emitted as a
  // visible replacement, even when the override carries a valid DTSTART.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:cancelled\r\nDTSTART:20240101T090000Z\r\n"
                            "DTEND:20240101T100000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:cancelled\r\nRECURRENCE-ID:20240102T090000Z\r\n"
                            "DTSTART:20240102T090000Z\r\nDTEND:20240102T100000Z\r\n"
                            "STATUS:CANCELLED\r\nEND:VEVENT\r\n";
    // cancelled recurrence override is not emitted
    TEST_CHECK(parsesToStarts(ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 3, 9)}));
  }

  // A malformed non-cancelled override without DTSTART is not a valid replacement and must not
  // suppress the corresponding occurrence from the master series.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:malformed-override\r\nDTSTART:20240101T090000Z\r\n"
                            "DTEND:20240101T100000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:malformed-override\r\n"
                            "RECURRENCE-ID:20240102T090000Z\r\nEND:VEVENT\r\n";
    // malformed recurrence override does not suppress master
    TEST_CHECK(parsesToStarts(ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 2, 9), utc(2024, 1, 3, 9)}));
  }

  // A cancelled override is a valid exclusion even when it omits DTSTART, as permitted for a
  // cancellation that identifies the original instance only by RECURRENCE-ID.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:cancelled-no-start\r\nDTSTART:20240101T090000Z\r\n"
                            "DTEND:20240101T100000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:cancelled-no-start\r\n"
                            "RECURRENCE-ID:20240102T090000Z\r\nSTATUS:CANCELLED\r\nEND:VEVENT\r\n";
    // cancelled recurrence override without dtstart excludes master
    TEST_CHECK(parsesToStarts(ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 3, 9)}));
  }

  // Overrides must only suppress the exact replaced occurrence. The old parser used a broad DST
  // tolerance for matching overrides; with libical's timezone-aware expansion, that tolerance would
  // wrongly remove neighbouring sub-daily recurrences.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:h\r\nDTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\n"
                            "RRULE:FREQ=HOURLY;COUNT=4\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:h\r\nRECURRENCE-ID:20240101T100000Z\r\n"
                            "DTSTART:20240101T150000Z\r\nDTEND:20240101T160000Z\r\nEND:VEVENT\r\n";
    // recurrence-id override only excludes exact occurrence
    TEST_CHECK(parsesToStarts(
        ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 1, 11), utc(2024, 1, 1, 12), utc(2024, 1, 1, 15)}
    ));
  }

  // Some CalDAV servers answer an expand request with per-instance components that still carry the
  // series RRULE. Each RECURRENCE-ID component is one occurrence, so it must not expand again.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:expanded\r\nDTSTART:20261020T110000Z\r\n"
                            "DTEND:20261020T115500Z\r\nRECURRENCE-ID:20261020T130000Z\r\n"
                            "RRULE:FREQ=WEEKLY;WKST=SU;BYDAY=TU\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:expanded\r\nDTSTART:20261027T110000Z\r\n"
                            "DTEND:20261027T115500Z\r\nRECURRENCE-ID:20261027T130000Z\r\n"
                            "RRULE:FREQ=WEEKLY;WKST=SU;BYDAY=TU\r\nEND:VEVENT\r\n";
    // expanded instance with rrule is not expanded again
    TEST_CHECK(
        parsesToStarts(ics, utc(2026, 10, 1), utc(2026, 11, 30), {utc(2026, 10, 20, 11), utc(2026, 10, 27, 11)})
    );
  }

  // Responses that are not iCalendar at all must be distinguishable from an empty calendar, so a
  // captive portal or expired share link cannot overwrite cached events with nothing.
  {
    const std::vector<std::string> invalid = {
        "<!DOCTYPE html>\r\n<html><body><h1>Sign in</h1></body></html>\r\n",
        R"({"error":"expired share"})",
        "not a calendar at all\r\n",
        "",
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:a\r\n",
        // The envelope survived but every content line inside the event was mangled, so nothing
        // usable came out. Distinct from a calendar that is legitimately empty.
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\n"
        "DTSTART 20240115T100000Z\r\nSUMMARY Team Meeting\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n",
    };
    for (const std::string& ics : invalid) {
      TEST_CHECK(parseEvents(ics, start, end).status == ICalParseStatus::InvalidCalendar);
    }

    const ICalParseResult empty = parseEvents("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nEND:VCALENDAR\r\n", start, end);
    TEST_CHECK(empty.status == ICalParseStatus::Complete);
    TEST_CHECK(empty.events.empty());
  }

  // Libical replaces unparseable content lines with X-LIC-ERROR properties instead of rejecting the
  // document, so a partially damaged feed still yields its readable events. Rejecting the whole
  // calendar here would let one quirky line hide every event in a large feed.
  {
    const std::string ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Broken Corp//Bad Calendar//EN\r\n"
                            "BEGIN:VEVENT\r\nUID:recovered\r\nDTSTART:20240115T100000Z\r\n"
                            "DTEND 20240115T110000Z\r\n"
                            "SUMMARY:Team Meeting\r\n"
                            "Description This line has no colon separator\r\n"
                            "END:VEVENT\r\nEND:VCALENDAR\r\n";
    // damaged lines are skipped and the event survives
    TEST_CHECK(parsesToOneEventText(ics, start, end, "Team Meeting", ""));
  }

  {
    ICalParseControl control{.remainingRecurrenceWork = 1};
    const ICalParseResult result = calendar::parseICalEvents(wrap("RRULE:FREQ=DAILY;COUNT=3\r\n"), start, end, control);
    TEST_CHECK(result.status == ICalParseStatus::WorkBudgetExceeded);
  }

  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:cancel\r\nDTSTART:20200101T000000Z\r\n"
                            "DTEND:20200101T000001Z\r\n"
                            "RRULE:FREQ=SECONDLY;COUNT=10000000\r\nEND:VEVENT\r\n";
    ICalParseResult result;
    const auto beforeCancellation = steady_clock::now();
    std::jthread parser([&](std::stop_token stopToken) {
      ICalParseControl control{
          .stopToken = std::move(stopToken),
          .remainingRecurrenceWork = 10'000'000,
      };
      result = calendar::parseICalEvents(ics, utc(2024, 1, 1), utc(2026, 1, 1), control);
    });
    std::this_thread::sleep_for(milliseconds{20});
    parser.request_stop();
    parser.join();
    TEST_CHECK(result.status == ICalParseStatus::Cancelled);
    TEST_CHECK(steady_clock::now() - beforeCancellation < milliseconds{500});
  }

  // ---- VALARM reminder extraction ----

  const auto leadsOf = [&](const std::string& vevent) {
    const std::string ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + vevent + "END:VCALENDAR\r\n";
    const ICalParseResult result = parseEvents(ics, utc(2024, 1, 1), utc(2024, 12, 31));
    return result.events.empty() ? std::optional<std::vector<std::int32_t>>{}
                                 : result.events.front().reminderLeadSeconds;
  };

  {
    const auto leads = leadsOf(
        "BEGIN:VEVENT\r\nUID:rel\r\nSUMMARY:Standup\r\nDTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\n"
        "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    );
    TEST_CHECK(leads == std::vector<std::int32_t>{900});
  }

  {
    // is_null_trigger() is true for PT0S, so gating on it would silently drop "alert at start".
    const auto leads = leadsOf(
        "BEGIN:VEVENT\r\nUID:zero\r\nSUMMARY:Now\r\nDTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\n"
        "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:PT0S\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    );
    TEST_CHECK(leads == std::vector<std::int32_t>{0});
  }

  {
    // RELATED=END anchors to DTEND, so on a 3 day event -P4D lands one day before DTSTART.
    const auto leads = leadsOf(
        "BEGIN:VEVENT\r\nUID:relend\r\nSUMMARY:Trip\r\nDTSTART:20240610T090000Z\r\nDTEND:20240613T090000Z\r\n"
        "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER;RELATED=END:-P4D\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    );
    TEST_CHECK(leads == std::vector<std::int32_t>{24 * 3600});
  }

  {
    // RELATED=END:-PT10M on a one hour event fires after the start, so it can never be relevant.
    const auto leads = leadsOf(
        "BEGIN:VEVENT\r\nUID:after\r\nSUMMARY:Late\r\nDTSTART:20240610T090000Z\r\nDTEND:20240610T100000Z\r\n"
        "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER;RELATED=END:-PT10M\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    );
    TEST_CHECK(!leads.has_value());
  }

  {
    const auto leads = leadsOf(
        "BEGIN:VEVENT\r\nUID:abs\r\nSUMMARY:One off\r\nDTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\n"
        "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER;VALUE=DATE-TIME:20240610T084500Z\r\n"
        "END:VALARM\r\nEND:VEVENT\r\n"
    );
    TEST_CHECK(leads == std::vector<std::int32_t>{900});
  }

  {
    // An absolute trigger fires once for a whole series, so it must not be copied onto occurrences.
    const std::string ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:absrec\r\nSUMMARY:Series\r\n"
                            "DTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\n"
                            "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER;VALUE=DATE-TIME:20240610T084500Z\r\n"
                            "END:VALARM\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    const ICalParseResult result = parseEvents(ics, utc(2024, 1, 1), utc(2024, 12, 31));
    bool allEmpty = !result.events.empty();
    for (const auto& event : result.events) {
      allEmpty = allEmpty && !event.reminderLeadSeconds.has_value();
    }
    TEST_CHECK(allEmpty);
  }

  {
    // Relative triggers must reach every expanded occurrence.
    const std::string ics =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:relrec\r\nSUMMARY:Series\r\n"
        "DTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\n"
        "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT5M\r\nEND:VALARM\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    const ICalParseResult result = parseEvents(ics, utc(2024, 1, 1), utc(2024, 12, 31));
    bool allCarry = result.events.size() == 3;
    for (const auto& event : result.events) {
      allCarry = allCarry && event.reminderLeadSeconds == std::vector<std::int32_t>{300};
    }
    TEST_CHECK(allCarry);
  }

  {
    const auto leads = leadsOf(
        "BEGIN:VEVENT\r\nUID:email\r\nSUMMARY:Mail\r\nDTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\n"
        "BEGIN:VALARM\r\nACTION:EMAIL\r\nTRIGGER:-PT30M\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    );
    TEST_CHECK(!leads.has_value());
  }

  {
    // ACTION is mandatory per RFC 5545, but a feed omitting it still means "alert me".
    const auto leads = leadsOf(
        "BEGIN:VEVENT\r\nUID:noaction\r\nSUMMARY:Bare\r\nDTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\n"
        "BEGIN:VALARM\r\nTRIGGER:-PT20M\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    );
    TEST_CHECK(leads == std::vector<std::int32_t>{1200});
  }

  {
    const auto leads = leadsOf(
        "BEGIN:VEVENT\r\nUID:multi\r\nSUMMARY:Many\r\nDTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\n"
        "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT30M\r\nEND:VALARM\r\n"
        "BEGIN:VALARM\r\nACTION:AUDIO\r\nTRIGGER:-PT5M\r\nEND:VALARM\r\n"
        "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT30M\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    );
    TEST_CHECK(leads == (std::vector<std::int32_t>{300, 1800}));
  }

  // ---- per-event colors ----

  {
    const std::string icsRfcColor = wrap("COLOR:#336699\r\n");
    const ICalParseResult res1 = parseEvents(icsRfcColor, start, end);
    TEST_CHECK(res1.status == ICalParseStatus::Complete);
    TEST_CHECK(!res1.events.empty());
    TEST_CHECK(res1.events[0].colorHex == "#336699");

    const std::string icsCssColor = wrap("COLOR:blue\r\n");
    const ICalParseResult res2 = parseEvents(icsCssColor, start, end);
    TEST_CHECK(res2.status == ICalParseStatus::Complete);
    TEST_CHECK(!res2.events.empty());
    TEST_CHECK(res2.events[0].colorHex == "#0000FF");

    const std::string icsAppleColor = wrap("X-APPLE-CALENDAR-COLOR:#FF5500\r\n");
    const ICalParseResult res3 = parseEvents(icsAppleColor, start, end);
    TEST_CHECK(res3.status == ICalParseStatus::Complete);
    TEST_CHECK(!res3.events.empty());
    TEST_CHECK(res3.events[0].colorHex == "#FF5500");

    const std::string icsAppleMixedCase = wrap("X-Apple-Calendar-Color:#FF5500\r\n");
    const ICalParseResult res4 = parseEvents(icsAppleMixedCase, start, end);
    TEST_CHECK(res4.status == ICalParseStatus::Complete);
    TEST_CHECK(!res4.events.empty());
    TEST_CHECK(res4.events[0].colorHex == "#FF5500");

    const std::string icsXColor = wrap("X-COLOR:#12AB34\r\n");
    const ICalParseResult res5 = parseEvents(icsXColor, start, end);
    TEST_CHECK(res5.status == ICalParseStatus::Complete);
    TEST_CHECK(!res5.events.empty());
    TEST_CHECK(res5.events[0].colorHex == "#12AB34");

    const std::string icsOutlookColor = wrap("X-OUTLOOK-COLOR:#A1B2C3\r\n");
    const ICalParseResult res6 = parseEvents(icsOutlookColor, start, end);
    TEST_CHECK(res6.status == ICalParseStatus::Complete);
    TEST_CHECK(!res6.events.empty());
    TEST_CHECK(res6.events[0].colorHex == "#A1B2C3");

    const std::string icsPrecedence = wrap("COLOR:blue\r\nX-COLOR:#FF0000\r\n");
    const ICalParseResult res7 = parseEvents(icsPrecedence, start, end);
    TEST_CHECK(res7.status == ICalParseStatus::Complete);
    TEST_CHECK(!res7.events.empty());
    TEST_CHECK(res7.events[0].colorHex == "#0000FF");

    const std::string icsInvalidRfcColor = wrap("COLOR:not-a-color\r\nX-COLOR:#0A0B0C\r\n");
    const ICalParseResult res8 = parseEvents(icsInvalidRfcColor, start, end);
    TEST_CHECK(res8.status == ICalParseStatus::Complete);
    TEST_CHECK(!res8.events.empty());
    TEST_CHECK(res8.events[0].colorHex == "#0A0B0C");

    const std::string icsModernNamedColor = wrap("COLOR:rebeccapurple\r\n");
    const ICalParseResult res9 = parseEvents(icsModernNamedColor, start, end);
    TEST_CHECK(res9.status == ICalParseStatus::Complete);
    TEST_CHECK(!res9.events.empty());
    TEST_CHECK(res9.events[0].colorHex == "#663399");

    const std::string icsNoColor = wrap("");
    const ICalParseResult res10 = parseEvents(icsNoColor, start, end);
    TEST_CHECK(res10.status == ICalParseStatus::Complete);
    TEST_CHECK(!res10.events.empty());
    TEST_CHECK(res10.events[0].colorHex.empty());
  }

  {
    std::string vevent =
        "BEGIN:VEVENT\r\nUID:cap\r\nSUMMARY:Capped\r\nDTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\n";
    for (int minutes = 1; minutes <= 8; ++minutes) {
      vevent += "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT" + std::to_string(minutes) + "M\r\nEND:VALARM\r\n";
    }
    vevent += "END:VEVENT\r\n";
    TEST_CHECK(leadsOf(vevent).value_or(std::vector<std::int32_t>{}).size() == calendar::kMaxRemindersPerEvent);
  }

  {
    const auto leads = leadsOf(
        "BEGIN:VEVENT\r\nUID:none\r\nSUMMARY:Quiet\r\nDTSTART:20240610T090000Z\r\nDTEND:20240610T093000Z\r\n"
        "END:VEVENT\r\n"
    );
    TEST_CHECK(!leads.has_value());
  }

  {
    Color color;
    TEST_CHECK(!tryParseCssColor("red", color));
    TEST_CHECK(tryParseCssColorWithNamedColors("ReBeccAPurple", color) && color == hex("#663399"));
  }

  return 0;
}
