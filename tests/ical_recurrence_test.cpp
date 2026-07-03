#include "calendar/ical_parser.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace {

  using namespace std::chrono;

  system_clock::time_point utc(int y, int mo, int d, int h = 0) {
    return sys_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}} + hours{h};
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

} // namespace

int main() {
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

  return ok ? 0 : 1;
}
