#include "calendar/ical_parser.h"
#include "render/core/color.h"

#include <algorithm>
#include <chrono>
#include <print>
#include <string>
#include <thread>
#include <vector>

namespace {

  using namespace std::chrono;
  using calendar::ICalParseControl;
  using calendar::ICalParseResult;
  using calendar::ICalParseStatus;

  system_clock::time_point utc(int y, int mo, int d, int h = 0) {
    return sys_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}} + hours{h};
  }

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "ical_parser_test: {}", message);
    }
    return condition;
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

  // Assert the parsed occurrences' start instants exactly match `expected` (order-independent).
  bool expectStarts(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end,
      std::vector<system_clock::time_point> expected, const char* message
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    if (result.status != ICalParseStatus::Complete) {
      std::println(stderr, "ical_parser_test: {}: parser did not complete", message);
      return false;
    }
    std::vector<system_clock::time_point> actual;
    actual.reserve(result.events.size());
    for (const auto& ev : result.events) {
      actual.push_back(ev.start);
    }
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    if (actual != expected) {
      std::println(stderr, "ical_parser_test: {}: expected {} starts, got {}", message, expected.size(), actual.size());
      for (const auto& t : actual) {
        std::println(stderr, "  got {}", static_cast<long long>(t.time_since_epoch().count()));
      }
      return false;
    }
    return true;
  }

  // A VEVENT starting Mon 2024-01-01 09:00 UTC (1h long), with the given extra property lines appended.
  std::string wrap(const std::string& props) {
    return "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:s\r\nDTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\n"
        + props
        + "END:VEVENT\r\n";
  }

  bool expectCount(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end, std::size_t expected,
      const char* message
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    if (result.status != ICalParseStatus::Complete) {
      std::println(stderr, "ical_parser_test: {}: parser did not complete", message);
      return false;
    }
    const std::size_t actual = result.events.size();
    if (actual != expected) {
      std::println(stderr, "ical_parser_test: {}: expected {}, got {}", message, expected, actual);
      return false;
    }
    return true;
  }

  bool expectRanges(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end,
      std::vector<std::pair<system_clock::time_point, system_clock::time_point>> expected, const char* message
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    if (result.status != ICalParseStatus::Complete) {
      std::println(stderr, "ical_parser_test: {}: parser did not complete", message);
      return false;
    }
    std::vector<std::pair<system_clock::time_point, system_clock::time_point>> actual;
    actual.reserve(result.events.size());
    for (const auto& ev : result.events) {
      actual.emplace_back(ev.start, ev.end);
    }
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    if (actual != expected) {
      std::println(stderr, "ical_parser_test: {}: expected {} ranges, got {}", message, expected.size(), actual.size());
      for (const auto& [s, e] : actual) {
        std::println(
            stderr, "  got {}..{}", static_cast<long long>(s.time_since_epoch().count()),
            static_cast<long long>(e.time_since_epoch().count())
        );
      }
      return false;
    }
    return true;
  }

  bool expectOneEventText(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end, const std::string& title,
      const std::string& location, const char* message
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    if (result.status != ICalParseStatus::Complete) {
      std::println(stderr, "ical_parser_test: {}: parser did not complete", message);
      return false;
    }
    const std::vector<CalendarEvent>& events = result.events;
    if (events.size() != 1 || events.front().title != title || events.front().location != location) {
      std::println(stderr, "ical_parser_test: {}: parsed text did not match", message);
      return false;
    }
    return true;
  }

  bool expectOneEventUrl(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end, const std::string& url,
      const char* message
  ) {
    ICalParseResult result = parseEvents(ics, start, end);
    if (result.status != ICalParseStatus::Complete || result.events.size() != 1) {
      std::println(stderr, "ical_parser_test: {}: expected exactly one parsed event", message);
      return false;
    }
    if (result.events.front().url != url) {
      std::println(
          stderr, R"(ical_parser_test: {}: url was "{}", expected "{}")", message, result.events.front().url, url
      );
      return false;
    }
    return true;
  }

} // namespace

int main() {
  const auto start = utc(2024, 1, 1);
  const auto end = utc(2024, 2, 1);
  bool ok = true;

  // No RRULE: exactly one instance passes through.
  ok = expectCount(wrap(""), start, end, 1, "non-recurring event") && ok;

  // Standard VCALENDAR wrappers and multiple VEVENT children are accepted.
  {
    const std::string ics =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
        "BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:b\r\nDTSTART:20240102T090000Z\r\nDTEND:20240102T100000Z\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    ok = expectCount(ics, start, end, 2, "vcalendar with multiple events") && ok;
  }

  // Libical handles folded content lines and RFC text escaping.
  {
    const std::string ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:text\r\n"
                            "SUMMARY:Planning\\n\r\n Session\r\n"
                            "LOCATION:Main\\, Room\r\n"
                            "DTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    ok = expectOneEventText(ics, start, end, "Planning\nSession", "Main, Room", "escaped text values") && ok;
  }

  // A meeting link in LOCATION becomes the event's clickable url, and wins over the URL property.
  {
    const std::string ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:meet\r\nSUMMARY:Daily Sync\r\n"
                            "LOCATION:https://meet.google.com/abc-defg-hij\r\n"
                            "URL:https://calendar.example/event/1\r\n"
                            "DTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    ok =
        expectOneEventUrl(ics, start, end, "https://meet.google.com/abc-defg-hij", "location link wins over url") && ok;
  }

  // With no link in LOCATION the URL property is used.
  {
    const std::string ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:url\r\nSUMMARY:Review\r\n"
                            "LOCATION:Meeting Room 3\r\n"
                            "URL:https://calendar.example/event/2\r\n"
                            "DTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    ok = expectOneEventUrl(ics, start, end, "https://calendar.example/event/2", "url property fallback") && ok;
  }

  // An event with neither carries no link.
  {
    const std::string ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:plain\r\nSUMMARY:Dentist\r\n"
                            "LOCATION:Kyiv\r\n"
                            "DTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    ok = expectOneEventUrl(ics, start, end, "", "no link") && ok;
  }

  // Malformed/incomplete VEVENTs are skipped instead of producing epoch-placeholder events.
  {
    const std::string ics =
        "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:bad\r\nSUMMARY:missing start\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    ok = expectCount(ics, start, end, 0, "event without dtstart is skipped") && ok;
  }

  // The Unix epoch is a valid event instant and must not be confused with an invalid timestamp.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:epoch\r\nSUMMARY:s\r\n"
                            "DTSTART:19700101T000000Z\r\nDTEND:19700101T010000Z\r\n"
                            "END:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(1969, 12, 31), utc(1970, 1, 2),
             {{system_clock::time_point{}, system_clock::time_point{} + hours{1}}}, "unix epoch event"
         )
        && ok;
  }

  // DAILY COUNT=3 -> 3 instances.
  ok = expectCount(wrap("RRULE:FREQ=DAILY;COUNT=3\r\n"), start, end, 3, "daily count") && ok;

  // RDATE contributes recurrence instances even when the component has no RRULE.
  ok = expectStarts(
           wrap("RDATE:20240102T090000Z\r\n"), start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 2, 9)},
           "rdate-only recurrence set"
       )
      && ok;

  // RRULE and RDATE are a set: an RDATE equal to an RRULE occurrence must not create a duplicate.
  ok = expectStarts(
           wrap("RRULE:FREQ=DAILY;COUNT=2\r\nRDATE:20240102T090000Z\r\n"), start, end,
           {utc(2024, 1, 1, 9), utc(2024, 1, 2, 9)}, "overlapping rrule and rdate are deduplicated"
       )
      && ok;

  // An explicit RDATE period overrides the master's duration when it shares an RRULE occurrence.
  ok = expectRanges(
           wrap(
               "RRULE:FREQ=DAILY;COUNT=2\r\n"
               "RDATE;VALUE=PERIOD:20240102T090000Z/20240102T120000Z\r\n"
           ),
           start, end, {{utc(2024, 1, 1, 9), utc(2024, 1, 1, 10)}, {utc(2024, 1, 2, 9), utc(2024, 1, 2, 12)}},
           "rdate period duration overrides rrule duration"
       )
      && ok;

  // An explicit RDATE period may express its end as a duration instead of an absolute timestamp.
  ok = expectRanges(
           wrap("RDATE;VALUE=PERIOD:20240102T090000Z/PT3H\r\n"), start, end,
           {{utc(2024, 1, 1, 9), utc(2024, 1, 1, 10)}, {utc(2024, 1, 2, 9), utc(2024, 1, 2, 12)}},
           "rdate period duration determines occurrence end"
       )
      && ok;

  // WEEKLY BYDAY=MO,WE, window 2024-01-01..2024-01-15 (excl): Mon 1, Wed 3, Mon 8, Wed 10 = 4.
  ok = expectCount(wrap("RRULE:FREQ=WEEKLY;BYDAY=MO,WE\r\n"), start, utc(2024, 1, 15), 4, "weekly byday") && ok;

  // WEEKLY INTERVAL=2 (no BYDAY): every other Monday from Jan 1 to Feb 1: Jan 1, 15, 29 = 3.
  ok = expectCount(wrap("RRULE:FREQ=WEEKLY;INTERVAL=2\r\n"), start, end, 3, "weekly interval") && ok;

  // MONTHLY unbounded over one year: Jan..Dec 2024 = 12.
  ok = expectCount(wrap("RRULE:FREQ=MONTHLY\r\n"), start, utc(2025, 1, 1), 12, "monthly") && ok;

  // A timed DTSTART with TZID must be converted exactly once. Kyiv is UTC+3 in July, so 11:00 local
  // is 08:00Z; treating the zoned wall time as UTC would display several hours late.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:kyiv-single\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=Europe/Kyiv:20240705T110000\r\n"
                            "DTEND;TZID=Europe/Kyiv:20240705T120000\r\n"
                            "END:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(2024, 7, 1), utc(2024, 8, 1), {{utc(2024, 7, 5, 8), utc(2024, 7, 5, 9)}},
             "timed Kyiv event converts timezone once"
         )
        && ok;
  }

  // Floating DATE-TIME values have no Z suffix and no TZID. They are local wall times, not UTC
  // instants, so 11:00 must be anchored through the user's current zone.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-single\r\nSUMMARY:s\r\n"
                            "DTSTART:20240705T110000\r\n"
                            "DTEND:20240705T120000\r\n"
                            "END:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(2024, 7, 1), utc(2024, 8, 1), {{localTime(2024, 7, 5, 11), localTime(2024, 7, 5, 12)}},
             "floating timed event uses local wall time"
         )
        && ok;
  }

  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-recurring\r\nSUMMARY:s\r\n"
                            "DTSTART:20240705T110000\r\n"
                            "DTEND:20240705T120000\r\nRRULE:FREQ=DAILY;COUNT=2\r\n"
                            "END:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(2024, 7, 1), utc(2024, 8, 1),
             {{localTime(2024, 7, 5, 11), localTime(2024, 7, 5, 12)},
              {localTime(2024, 7, 6, 11), localTime(2024, 7, 6, 12)}},
             "floating timed recurrence uses local wall time"
         )
        && ok;
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
    ok = expectStarts(
             ics, occurrence - minutes{30}, occurrence + minutes{90}, {occurrence},
             "floating recurrence within UTC window boundary"
         )
        && ok;
  }

  // Floating recurrences remain local wall times across the system zone's DST boundary. In
  // Europe/Kyiv this crosses the Mar 31 2024 spring-forward transition; in zones without that
  // transition the expectation still verifies local anchoring rather than UTC anchoring.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-dst\r\nSUMMARY:s\r\n"
                            "DTSTART:20240329T110000\r\n"
                            "DTEND:20240329T120000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                            "END:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(2024, 3, 28), utc(2024, 4, 4),
             {{localTime(2024, 3, 29, 11), localTime(2024, 3, 29, 12)},
              {localTime(2024, 3, 30, 11), localTime(2024, 3, 30, 12)},
              {localTime(2024, 3, 31, 11), localTime(2024, 3, 31, 12)},
              {localTime(2024, 4, 1, 11), localTime(2024, 4, 1, 12)},
              {localTime(2024, 4, 2, 11), localTime(2024, 4, 2, 12)}},
             "floating timed recurrence preserves local wall time across dst"
         )
        && ok;
  }

  // Timed monthly recurrences with TZID must preserve the local wall time across DST. Kyiv is UTC+2
  // in February and UTC+3 in July, so an 11:00 February event should recur at 08:00Z in July.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:kyiv-monthly\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=Europe/Kyiv:20240205T110000\r\n"
                            "DTEND;TZID=Europe/Kyiv:20240205T120000\r\nRRULE:FREQ=MONTHLY;COUNT=6\r\n"
                            "END:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(2024, 7, 1), utc(2024, 8, 1), {{utc(2024, 7, 5, 8), utc(2024, 7, 5, 9)}},
             "monthly timed recurrence preserves Kyiv wall time across dst"
         )
        && ok;
  }

  // Daily TZID recurrences preserve local wall time across DST. New York switches from UTC-5 to
  // UTC-4 on Mar 10 2024, so the UTC instant moves from 14:00Z to 13:00Z while local time stays 09:00.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ny-daily-dst\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=America/New_York:20240308T090000\r\n"
                            "DTEND;TZID=America/New_York:20240308T100000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                            "END:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(2024, 3, 8), utc(2024, 3, 14),
             {{utc(2024, 3, 8, 14), utc(2024, 3, 8, 15)},
              {utc(2024, 3, 9, 14), utc(2024, 3, 9, 15)},
              {utc(2024, 3, 10, 13), utc(2024, 3, 10, 14)},
              {utc(2024, 3, 11, 13), utc(2024, 3, 11, 14)},
              {utc(2024, 3, 12, 13), utc(2024, 3, 12, 14)}},
             "daily timed recurrence preserves New York wall time across dst"
         )
        && ok;
  }

  // Monthly recurrences on a month-end date skip invalid civil dates instead of approximating with a
  // fixed day count. Jan 31 + MONTHLY yields Jan 31, Mar 31, May 31, Jul 31... because February and
  // April have no 31st.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:month-end\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=Europe/Kyiv:20240131T110000\r\n"
                            "DTEND;TZID=Europe/Kyiv:20240131T120000\r\nRRULE:FREQ=MONTHLY;COUNT=4\r\n"
                            "END:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(2024, 1, 1), utc(2024, 6, 15),
             {{utc(2024, 1, 31, 9), utc(2024, 1, 31, 10)},
              {utc(2024, 3, 31, 8), utc(2024, 3, 31, 9)},
              {utc(2024, 5, 31, 8), utc(2024, 5, 31, 9)}},
             "monthly timed recurrence skips invalid month-end dates"
         )
        && ok;
  }

  // UNTIL clips: DAILY until Jan 3 -> Jan 1, 2, 3 = 3.
  ok = expectCount(wrap("RRULE:FREQ=DAILY;UNTIL=20240103T090000Z\r\n"), start, end, 3, "daily until") && ok;

  // UTC UNTIL clips a TZID recurrence by instant. Mar 10 09:00 New York is 13:00Z after the DST jump,
  // so an UNTIL at 13:00Z includes Mar 8, Mar 9, and Mar 10.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ny-until\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=America/New_York:20240308T090000\r\n"
                            "DTEND;TZID=America/New_York:20240308T100000\r\n"
                            "RRULE:FREQ=DAILY;UNTIL=20240310T130000Z\r\n"
                            "END:VEVENT\r\n";
    ok = expectStarts(
             ics, utc(2024, 3, 8), utc(2024, 3, 14), {utc(2024, 3, 8, 14), utc(2024, 3, 9, 14), utc(2024, 3, 10, 13)},
             "utc until clips timezone recurrence by instant"
         )
        && ok;
  }

  // EXDATE drops one occurrence: DAILY COUNT=5 minus Jan 3 = 4.
  ok = expectCount(wrap("RRULE:FREQ=DAILY;COUNT=5\r\nEXDATE:20240103T090000Z\r\n"), start, end, 4, "exdate") && ok;

  // Floating EXDATE values match floating recurrence instances after local wall-time anchoring.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-exdate\r\nSUMMARY:s\r\n"
                            "DTSTART:20240705T110000\r\n"
                            "DTEND:20240705T120000\r\nRRULE:FREQ=DAILY;COUNT=3\r\n"
                            "EXDATE:20240706T110000\r\nEND:VEVENT\r\n";
    ok = expectStarts(
             ics, utc(2024, 7, 1), utc(2024, 8, 1), {localTime(2024, 7, 5, 11), localTime(2024, 7, 7, 11)},
             "floating exdate excludes local wall-time occurrence"
         )
        && ok;
  }

  // Multiple EXDATE values on one property are common in exported calendars and must exclude each
  // listed occurrence.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:multi-exdate\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=Europe/Kyiv:20240705T110000\r\n"
                            "DTEND;TZID=Europe/Kyiv:20240705T120000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                            "EXDATE;TZID=Europe/Kyiv:20240706T110000,20240708T110000\r\nEND:VEVENT\r\n";
    ok = expectStarts(
             ics, utc(2024, 7, 1), utc(2024, 8, 1), {utc(2024, 7, 5, 8), utc(2024, 7, 7, 8), utc(2024, 7, 9, 8)},
             "multiple exdate values exclude listed occurrences"
         )
        && ok;
  }

  // Window clips leading occurrences but COUNT still counts them: DAILY COUNT=10 from Dec 30 2023,
  // window opens Jan 1 -> Dec 30/31 not shown, Jan 1..8 shown = 8.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:y\r\nDTSTART:20231230T090000Z\r\nDTEND:20231230T100000Z\r\n"
                            "RRULE:FREQ=DAILY;COUNT=10\r\nEND:VEVENT\r\n";
    ok = expectCount(ics, start, end, 8, "count spans window start") && ok;
  }

  // Unbounded daily series starting far before the window (2005) must still fill the whole Jan 2024
  // month window: 31 days. Guards against the iteration cap truncating an old series to nothing.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:z\r\nDTSTART:20050101T090000Z\r\nDTEND:20050101T100000Z\r\n"
                            "RRULE:FREQ=DAILY\r\nEND:VEVENT\r\n";
    ok = expectCount(ics, start, end, 31, "old unbounded daily reaches window") && ok;
  }

  // An explicit COUNT must not disable the skip-ahead: a bounded daily series whose DTSTART is many
  // years (>4000 days) before the window still has to reach it. COUNT=6000 from 2013 spans past 2024,
  // so the whole Jan window shows = 31 days. Guards against COUNT truncating an old series to nothing.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:cnt\r\nDTSTART:20130101T090000Z\r\nDTEND:20130101T100000Z\r\n"
                            "RRULE:FREQ=DAILY;COUNT=6000\r\nEND:VEVENT\r\n";
    ok = expectCount(ics, start, end, 31, "old daily with count reaches window") && ok;
  }

  // All-day (VALUE=DATE) MONTHLY on the 1st, over a one-year window, is exactly 12 - one per month.
  // The old code derived the day-of-month from floor<days> of the UTC instant, which for a zone east
  // of UTC rolls back to the previous civil day (the 31st of Dec), so months without a 31st were
  // dropped. Correct behavior is zone-independent. Window is padded ±half-month so each local-midnight
  // occurrence lands inside regardless of the running zone's UTC offset.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ad\r\nSUMMARY:s\r\nDTSTART;VALUE=DATE:20240101\r\n"
                            "DTEND;VALUE=DATE:20240102\r\nRRULE:FREQ=MONTHLY\r\nEND:VEVENT\r\n";
    ok = expectCount(ics, utc(2023, 12, 15), utc(2024, 12, 15), 12, "all-day monthly civil date") && ok;
  }

  // All-day recurrences are also local-valued. In Europe/Kyiv, July 6 local midnight is July 5
  // 21:00Z and must be expanded when a UTC window covers that instant.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:all-day-window-boundary\r\nSUMMARY:s\r\n"
                            "DTSTART;VALUE=DATE:20240706\r\n"
                            "DTEND;VALUE=DATE:20240707\r\nRRULE:FREQ=DAILY;COUNT=2\r\n"
                            "END:VEVENT\r\n";
    const system_clock::time_point occurrence = localMidnight(2024, 7, 6);
    ok = expectStarts(
             ics, occurrence - hours{1}, occurrence + hours{1}, {occurrence},
             "all-day recurrence within UTC window boundary"
         )
        && ok;
  }

  // Multi-day all-day recurrences must preserve their civil-day span, not a fixed UTC duration.
  // The May occurrence must span its own local midnights (May 30 -> Jun 1), computed against the
  // running zone. In a zone whose DST starts on Mar 31 (e.g. Europe/Kyiv), the Mar 30->Apr 1 master
  // is 47 UTC hours, so applying that fixed duration would end the May instance an hour early; the
  // civil-day span keeps it correct. Expectations track current_zone() so this holds in any zone.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:dst\r\nSUMMARY:s\r\nDTSTART;VALUE=DATE:20240330\r\n"
                            "DTEND;VALUE=DATE:20240401\r\nRRULE:FREQ=MONTHLY;COUNT=3\r\nEND:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(2024, 5, 1), utc(2024, 6, 15), {{localMidnight(2024, 5, 30), localMidnight(2024, 6, 1)}},
             "all-day monthly preserves civil end across dst"
         )
        && ok;
  }

  // EXDATE must still exclude across a DST boundary. Occurrences hold a constant UTC instant (drifting
  // ~1h vs local wall time across DST), while the server's EXDATE carries the true local wall time - a
  // different instant. TZID makes the drift zone-independent: New York DST starts Mar 10 2024, so a
  // daily noon event with an EXDATE on Mar 15 (post-DST) must drop exactly that day: 20 - 1 = 19.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:dstx\r\nDTSTART;TZID=America/New_York:20240301T120000\r\n"
                            "DTEND;TZID=America/New_York:20240301T130000\r\nRRULE:FREQ=DAILY;COUNT=20\r\n"
                            "EXDATE;TZID=America/New_York:20240315T120000\r\nEND:VEVENT\r\n";
    ok = expectCount(ics, utc(2024, 3, 1), utc(2024, 4, 1), 19, "exdate excludes across dst") && ok;
  }

  // Daily timed recurrences must not crash when an occurrence lands in a spring-forward gap. New York
  // jumps from 01:59 to 03:00 on Mar 10 2024, so 02:30 does not exist that day.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ny-gap\r\nDTSTART;TZID=America/New_York:20240308T023000\r\n"
                            "DTEND;TZID=America/New_York:20240308T033000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                            "END:VEVENT\r\n";
    ok = expectCount(ics, utc(2024, 3, 8), utc(2024, 3, 14), 5, "daily recurrence survives dst gap") && ok;
  }

  // Fall-back ambiguous local times must produce a deterministic event instead of a duplicate or a
  // dropped occurrence. New York 2024-11-03 01:30 occurs twice when clocks fall back.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ny-fold\r\nSUMMARY:s\r\n"
                            "DTSTART;TZID=America/New_York:20241103T013000\r\n"
                            "DTEND;TZID=America/New_York:20241103T023000\r\n"
                            "END:VEVENT\r\n";
    ok = expectCount(ics, utc(2024, 11, 3), utc(2024, 11, 4), 1, "ambiguous fall-back time parses once") && ok;
  }

  // RECURRENCE-ID override: a modified instance replaces the master's occurrence at that instant,
  // it does not add a duplicate. Master DAILY COUNT=3 (Jan 1/2/3 @09:00); the override moves Jan 3
  // to 14:00. Expect Jan 1 @09, Jan 2 @09, Jan 3 @14 - not four events, and no leftover Jan 3 @09.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:x\r\nDTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\n"
                            "RRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:x\r\nRECURRENCE-ID:20240103T090000Z\r\n"
                            "DTSTART:20240103T140000Z\r\nDTEND:20240103T150000Z\r\nEND:VEVENT\r\n";
    ok = expectStarts(
             ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 2, 9), utc(2024, 1, 3, 14)},
             "recurrence-id override replaces occurrence"
         )
        && ok;
  }

  // Floating RECURRENCE-ID values replace the matching local wall-time occurrence, not the same
  // numeric UTC wall fields.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:floating-override\r\nSUMMARY:s\r\n"
                            "DTSTART:20240705T110000\r\nDTEND:20240705T120000\r\n"
                            "RRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:floating-override\r\nRECURRENCE-ID:20240706T110000\r\n"
                            "DTSTART:20240706T150000\r\nDTEND:20240706T160000\r\nEND:VEVENT\r\n";
    ok = expectStarts(
             ics, utc(2024, 7, 1), utc(2024, 8, 1),
             {localTime(2024, 7, 5, 11), localTime(2024, 7, 6, 15), localTime(2024, 7, 7, 11)},
             "floating recurrence-id override replaces local wall-time occurrence"
         )
        && ok;
  }

  // A cancelled override removes the matching master occurrence and must not be emitted as a
  // visible replacement, even when the override carries a valid DTSTART.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:cancelled\r\nDTSTART:20240101T090000Z\r\n"
                            "DTEND:20240101T100000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:cancelled\r\nRECURRENCE-ID:20240102T090000Z\r\n"
                            "DTSTART:20240102T090000Z\r\nDTEND:20240102T100000Z\r\n"
                            "STATUS:CANCELLED\r\nEND:VEVENT\r\n";
    ok = expectStarts(
             ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 3, 9)}, "cancelled recurrence override is not emitted"
         )
        && ok;
  }

  // A malformed non-cancelled override without DTSTART is not a valid replacement and must not
  // suppress the corresponding occurrence from the master series.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:malformed-override\r\nDTSTART:20240101T090000Z\r\n"
                            "DTEND:20240101T100000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:malformed-override\r\n"
                            "RECURRENCE-ID:20240102T090000Z\r\nEND:VEVENT\r\n";
    ok = expectStarts(
             ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 2, 9), utc(2024, 1, 3, 9)},
             "malformed recurrence override does not suppress master"
         )
        && ok;
  }

  // A cancelled override is a valid exclusion even when it omits DTSTART, as permitted for a
  // cancellation that identifies the original instance only by RECURRENCE-ID.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:cancelled-no-start\r\nDTSTART:20240101T090000Z\r\n"
                            "DTEND:20240101T100000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:cancelled-no-start\r\n"
                            "RECURRENCE-ID:20240102T090000Z\r\nSTATUS:CANCELLED\r\nEND:VEVENT\r\n";
    ok = expectStarts(
             ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 3, 9)},
             "cancelled recurrence override without dtstart excludes master"
         )
        && ok;
  }

  // Overrides must only suppress the exact replaced occurrence. The old parser used a broad DST
  // tolerance for matching overrides; with libical's timezone-aware expansion, that tolerance would
  // wrongly remove neighbouring sub-daily recurrences.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:h\r\nDTSTART:20240101T090000Z\r\nDTEND:20240101T100000Z\r\n"
                            "RRULE:FREQ=HOURLY;COUNT=4\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:h\r\nRECURRENCE-ID:20240101T100000Z\r\n"
                            "DTSTART:20240101T150000Z\r\nDTEND:20240101T160000Z\r\nEND:VEVENT\r\n";
    ok = expectStarts(
             ics, start, end, {utc(2024, 1, 1, 9), utc(2024, 1, 1, 11), utc(2024, 1, 1, 12), utc(2024, 1, 1, 15)},
             "recurrence-id override only excludes exact occurrence"
         )
        && ok;
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
      ok = expect(
               parseEvents(ics, start, end).status == ICalParseStatus::InvalidCalendar,
               "non-calendar response was not reported as invalid"
           )
          && ok;
    }

    const ICalParseResult empty = parseEvents("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nEND:VCALENDAR\r\n", start, end);
    ok = expect(empty.status == ICalParseStatus::Complete, "empty calendar was not reported as complete") && ok;
    ok = expect(empty.events.empty(), "empty calendar produced events") && ok;
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
    ok = expectOneEventText(ics, start, end, "Team Meeting", "", "damaged lines are skipped and the event survives")
        && ok;
  }

  {
    ICalParseControl control{.remainingRecurrenceWork = 1};
    const ICalParseResult result = calendar::parseICalEvents(wrap("RRULE:FREQ=DAILY;COUNT=3\r\n"), start, end, control);
    ok = expect(
             result.status == ICalParseStatus::WorkBudgetExceeded,
             "recurrence work exhaustion did not report its explicit status"
         )
        && ok;
  }

  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:cancel\r\nDTSTART:20200101T000000Z\r\n"
                            "DTEND:20200101T000001Z\r\n"
                            "RRULE:FREQ=SECONDLY;COUNT=10000000\r\nEND:VEVENT\r\n";
    ICalParseResult result;
    const auto beforeCancellation = steady_clock::now();
    std::jthread parser([&](std::stop_token stopToken) {
      ICalParseControl control{
          .stopToken = stopToken,
          .remainingRecurrenceWork = 10'000'000,
      };
      result = calendar::parseICalEvents(ics, utc(2024, 1, 1), utc(2026, 1, 1), control);
    });
    std::this_thread::sleep_for(milliseconds{20});
    parser.request_stop();
    parser.join();
    ok = expect(result.status == ICalParseStatus::Cancelled, "stop request did not cancel recurrence expansion") && ok;
    ok = expect(
             steady_clock::now() - beforeCancellation < milliseconds{500},
             "cancelled recurrence expansion did not stop promptly"
         )
        && ok;
  }

  {
    const std::string icsRfcColor = wrap("COLOR:#336699\r\n");
    const ICalParseResult res1 = parseEvents(icsRfcColor, start, end);
    ok = expect(res1.status == ICalParseStatus::Complete, "RFC COLOR parse failed") && ok;
    ok = expect(!res1.events.empty() && res1.events[0].colorHex == "#336699", "RFC COLOR not extracted") && ok;

    const std::string icsCssColor = wrap("COLOR:blue\r\n");
    const ICalParseResult res2 = parseEvents(icsCssColor, start, end);
    ok = expect(res2.status == ICalParseStatus::Complete, "CSS COLOR parse failed") && ok;
    ok = expect(!res2.events.empty() && res2.events[0].colorHex == "#0000FF", "CSS COLOR not converted to hex") && ok;

    const std::string icsAppleColor = wrap("X-APPLE-CALENDAR-COLOR:#FF5500\r\n");
    const ICalParseResult res3 = parseEvents(icsAppleColor, start, end);
    ok = expect(res3.status == ICalParseStatus::Complete, "Apple color parse failed") && ok;
    ok = expect(!res3.events.empty() && res3.events[0].colorHex == "#FF5500", "Apple color not extracted") && ok;

    const std::string icsAppleMixedCase = wrap("X-Apple-Calendar-Color:#FF5500\r\n");
    const ICalParseResult res4 = parseEvents(icsAppleMixedCase, start, end);
    ok = expect(res4.status == ICalParseStatus::Complete, "mixed-case Apple color parse failed") && ok;
    ok = expect(!res4.events.empty() && res4.events[0].colorHex == "#FF5500", "mixed-case Apple color not extracted")
        && ok;

    const std::string icsXColor = wrap("X-COLOR:#12AB34\r\n");
    const ICalParseResult res5 = parseEvents(icsXColor, start, end);
    ok = expect(res5.status == ICalParseStatus::Complete, "X-COLOR parse failed") && ok;
    ok = expect(!res5.events.empty() && res5.events[0].colorHex == "#12AB34", "X-COLOR not extracted") && ok;

    const std::string icsOutlookColor = wrap("X-OUTLOOK-COLOR:#A1B2C3\r\n");
    const ICalParseResult res6 = parseEvents(icsOutlookColor, start, end);
    ok = expect(res6.status == ICalParseStatus::Complete, "X-OUTLOOK-COLOR parse failed") && ok;
    ok = expect(!res6.events.empty() && res6.events[0].colorHex == "#A1B2C3", "X-OUTLOOK-COLOR not extracted") && ok;

    const std::string icsPrecedence = wrap("COLOR:blue\r\nX-COLOR:#FF0000\r\n");
    const ICalParseResult res7 = parseEvents(icsPrecedence, start, end);
    ok = expect(res7.status == ICalParseStatus::Complete, "color precedence parse failed") && ok;
    ok = expect(!res7.events.empty() && res7.events[0].colorHex == "#0000FF", "RFC COLOR did not win") && ok;

    const std::string icsInvalidRfcColor = wrap("COLOR:not-a-color\r\nX-COLOR:#0A0B0C\r\n");
    const ICalParseResult res8 = parseEvents(icsInvalidRfcColor, start, end);
    ok = expect(res8.status == ICalParseStatus::Complete, "invalid RFC color parse failed") && ok;
    ok = expect(
             !res8.events.empty() && res8.events[0].colorHex == "#0A0B0C",
             "vendor color was not used after invalid RFC COLOR"
         )
        && ok;

    const std::string icsModernNamedColor = wrap("COLOR:rebeccapurple\r\n");
    const ICalParseResult res9 = parseEvents(icsModernNamedColor, start, end);
    ok = expect(res9.status == ICalParseStatus::Complete, "modern named color parse failed") && ok;
    ok = expect(!res9.events.empty() && res9.events[0].colorHex == "#663399", "modern named color was not converted")
        && ok;

    const std::string icsNoColor = wrap("");
    const ICalParseResult res10 = parseEvents(icsNoColor, start, end);
    ok = expect(res10.status == ICalParseStatus::Complete, "No-color parse failed") && ok;
    ok = expect(!res10.events.empty() && res10.events[0].colorHex.empty(), "No-color event has non-empty colorHex")
        && ok;
  }

  {
    Color color;
    ok = expect(!tryParseCssColor("red", color), "strict CSS parser accepted a named color") && ok;
    ok = expect(
             tryParseCssColorWithNamedColors("ReBeccAPurple", color) && color == hex("#663399"),
             "named CSS parser rejected rebeccapurple"
         )
        && ok;
  }

  return ok ? 0 : 1;
}
