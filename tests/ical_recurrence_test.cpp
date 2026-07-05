#include "calendar/ical_parser.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

  using namespace std::chrono;

  system_clock::time_point utc(int y, int mo, int d, int h = 0) {
    return sys_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}} + hours{h};
  }

  // Assert the parsed occurrences' start instants exactly match `expected` (order-independent).
  bool expectStarts(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end,
      std::vector<system_clock::time_point> expected, const char* message
  ) {
    std::vector<system_clock::time_point> actual;
    for (const auto& ev : calendar::parseICalEvents(ics, start, end)) {
      actual.push_back(ev.start);
    }
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    if (actual != expected) {
      std::fprintf(
          stderr, "ical_recurrence_test: %s: expected %zu starts, got %zu\n", message, expected.size(), actual.size()
      );
      for (const auto& t : actual) {
        std::fprintf(stderr, "  got %lld\n", static_cast<long long>(t.time_since_epoch().count()));
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
    const std::size_t actual = calendar::parseICalEvents(ics, start, end).size();
    if (actual != expected) {
      std::fprintf(stderr, "ical_recurrence_test: %s: expected %zu, got %zu\n", message, expected, actual);
      return false;
    }
    return true;
  }

  bool expectRanges(
      const std::string& ics, system_clock::time_point start, system_clock::time_point end,
      std::vector<std::pair<system_clock::time_point, system_clock::time_point>> expected, const char* message
  ) {
    std::vector<std::pair<system_clock::time_point, system_clock::time_point>> actual;
    for (const auto& ev : calendar::parseICalEvents(ics, start, end)) {
      actual.emplace_back(ev.start, ev.end);
    }
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    if (actual != expected) {
      std::fprintf(
          stderr, "ical_recurrence_test: %s: expected %zu ranges, got %zu\n", message, expected.size(), actual.size()
      );
      for (const auto& [s, e] : actual) {
        std::fprintf(
            stderr, "  got %lld..%lld\n", static_cast<long long>(s.time_since_epoch().count()),
            static_cast<long long>(e.time_since_epoch().count())
        );
      }
      return false;
    }
    return true;
  }

} // namespace

int main() {
  setenv("TZ", "Europe/Kyiv", 1);

  const auto start = utc(2024, 1, 1);
  const auto end = utc(2024, 2, 1);
  bool ok = true;

  // No RRULE: exactly one instance passes through.
  ok = expectCount(wrap(""), start, end, 1, "non-recurring event") && ok;

  // DAILY COUNT=3 -> 3 instances.
  ok = expectCount(wrap("RRULE:FREQ=DAILY;COUNT=3\r\n"), start, end, 3, "daily count") && ok;

  // WEEKLY BYDAY=MO,WE, window 2024-01-01..2024-01-15 (excl): Mon 1, Wed 3, Mon 8, Wed 10 = 4.
  ok = expectCount(wrap("RRULE:FREQ=WEEKLY;BYDAY=MO,WE\r\n"), start, utc(2024, 1, 15), 4, "weekly byday") && ok;

  // WEEKLY INTERVAL=2 (no BYDAY): every other Monday from Jan 1 to Feb 1: Jan 1, 15, 29 = 3.
  ok = expectCount(wrap("RRULE:FREQ=WEEKLY;INTERVAL=2\r\n"), start, end, 3, "weekly interval") && ok;

  // MONTHLY unbounded over one year: Jan..Dec 2024 = 12.
  ok = expectCount(wrap("RRULE:FREQ=MONTHLY\r\n"), start, utc(2025, 1, 1), 12, "monthly") && ok;

  // UNTIL clips: DAILY until Jan 3 -> Jan 1, 2, 3 = 3.
  ok = expectCount(wrap("RRULE:FREQ=DAILY;UNTIL=20240103T090000Z\r\n"), start, end, 3, "daily until") && ok;

  // EXDATE drops one occurrence: DAILY COUNT=5 minus Jan 3 = 4.
  ok = expectCount(wrap("RRULE:FREQ=DAILY;COUNT=5\r\nEXDATE:20240103T090000Z\r\n"), start, end, 4, "exdate") && ok;

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

  // All-day (VALUE=DATE) MONTHLY on the 1st, over a one-year window, is exactly 12 - one per month.
  // The old code derived the day-of-month from floor<days> of the UTC instant, which for a zone east
  // of UTC rolls back to the previous civil day (the 31st of Dec), so months without a 31st were
  // dropped. Correct behaviour is zone-independent. Window is padded ±half-month so each local-midnight
  // occurrence lands inside regardless of the running zone's UTC offset.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:ad\r\nSUMMARY:s\r\nDTSTART;VALUE=DATE:20240101\r\n"
                            "DTEND;VALUE=DATE:20240102\r\nRRULE:FREQ=MONTHLY\r\nEND:VEVENT\r\n";
    ok = expectCount(ics, utc(2023, 12, 15), utc(2024, 12, 15), 12, "all-day monthly civil date") && ok;
  }

  // Multi-day all-day recurrences must preserve their civil-day span, not a fixed UTC duration.
  // 2024-03-30..2024-04-01 in Europe/Kyiv is 47 UTC hours because DST starts on Mar 31; applying
  // that duration to the May occurrence would end it at 23:00 local on May 31 instead of midnight Jun 1.
  {
    const std::string ics = "BEGIN:VEVENT\r\nUID:dst\r\nSUMMARY:s\r\nDTSTART;VALUE=DATE:20240330\r\n"
                            "DTEND;VALUE=DATE:20240401\r\nRRULE:FREQ=MONTHLY;COUNT=3\r\nEND:VEVENT\r\n";
    ok = expectRanges(
             ics, utc(2024, 5, 1), utc(2024, 6, 15), {{utc(2024, 5, 29, 21), utc(2024, 5, 31, 21)}},
             "all-day monthly preserves civil end across dst"
         )
        && ok;
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

  return ok ? 0 : 1;
}
