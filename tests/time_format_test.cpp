#include "i18n/i18n_service.h"
#include "tests/test_check.h"
#include "time/time_format.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

namespace {

  std::string expectedZoneLabel(const std::chrono::sys_seconds& now, const std::chrono::time_zone& zone) {
    const auto info = zone.get_info(now);
    const auto totalMinutes = std::chrono::duration_cast<std::chrono::minutes>(info.offset).count();
    const auto hours = totalMinutes / 60;
    const auto minutes = std::abs(totalMinutes % 60);
    return std::format("{:+03}{:02}|{}", hours, minutes, info.abbrev);
  }

  std::string utcDayOfYear(const std::chrono::sys_seconds& now) {
    const auto today = std::chrono::floor<std::chrono::days>(now);
    const std::chrono::year_month_day ymd{today};
    const auto dayOfYear = (today - std::chrono::sys_days{ymd.year() / std::chrono::January / 1}).count() + 1;
    return std::format("{:03}", dayOfYear);
  }

} // namespace

int main() {
  using namespace std::chrono;

  i18n::Service::instance().init("en");

  TEST_CHECK(formatLocalUnixTime(1700000000, "").empty());
  TEST_CHECK(formatLocalTime("").empty());
  TEST_CHECK(formatLocalTime(nullptr).empty());
  TEST_CHECK(formatTimezoneTime("", "UTC").empty());
  TEST_CHECK(formatLocalUnixTime(1700000000, "%s") == "1700000000");
  TEST_CHECK(formatLocalUnixTime(1700000000, "recording_%s") == "recording_1700000000");
  TEST_CHECK(formatLocalUnixTime(1700000000, "%%s_%s") == "%s_1700000000");
  // Empty means system-local timezone.
  TEST_CHECK(isValidTimezone(""));
  TEST_CHECK(isValidTimezone("UTC"));
  TEST_CHECK(!isValidTimezone("Europe/Berln"));
  const auto beforeTimezoneFormat = floor<seconds>(system_clock::now());
  const auto* kiritimati = locate_zone("Pacific/Kiritimati");
  TEST_CHECK(formatTimezoneTime("%z|%Z", kiritimati->name()) == expectedZoneLabel(beforeTimezoneFormat, *kiritimati));
  // The zone abbreviation renders the same in a chrono field as in a bare spec.
  TEST_CHECK(formatTimezoneTime("{:%Z}", kiritimati->name()) == formatTimezoneTime("%Z", kiritimati->name()));
  const std::string formattedUtcDay = formatTimezoneTime("%j", "UTC");
  const auto afterTimezoneFormat = floor<seconds>(system_clock::now());
  TEST_CHECK(
      formattedUtcDay == utcDayOfYear(beforeTimezoneFormat) || formattedUtcDay == utcDayOfYear(afterTimezoneFormat)
  );
  TEST_CHECK(formatDuration(59s) == "<1m");
  TEST_CHECK(formatDuration(1min) == "1 minute");
  TEST_CHECK(formatDuration(2h + 1min) == "2 hours 1 minute");
  TEST_CHECK(formatDuration(24h + 1h + 1min) == "1 day 1 hour 1 minute");

  // Both rendering paths must report the same local time: a %-spec goes through strftime, a bare
  // {} through std::format. Deriving the offset from one clock keeps them on the configured zone.
  constexpr std::int64_t kFixedStamp = 1700000000; // 2023-11-14T22:13:20Z, +05:45 in Kathmandu.
  ::setenv("TZ", "UTC", 1);
  ::tzset();
  TEST_CHECK(formatLocalUnixTime(kFixedStamp, "%Y-%m-%d %H:%M:%S") == "2023-11-14 22:13:20");

  // A timezone change must reach an already-running process without requiring its caller to tzset.
  ::setenv("TZ", "Asia/Kathmandu", 1);
  TEST_CHECK(formatLocalUnixTime(kFixedStamp, "%Y-%m-%d %H:%M:%S") == "2023-11-15 03:58:20");
  TEST_CHECK(formatLocalUnixTime(kFixedStamp, "{}") == "2023-11-15 03:58:20");
  ::setenv("TZ", "UTC", 1);
  TEST_CHECK(formatLocalTime("%z") == "+0000");

  // Every directive that changes each second must request second ticks, in any
  // spelling the formatter accepts; minute-level and escaped ones must not.
  for (const char* fmt :
       {"{:%H:%M:%S}", "%T", "{:%X}", "%r", "{:%c}", "%s", "{:%OS}", "%EX", "%Ec", "%-S", "%_2S", "%H:%M\\n%S"}) {
    TEST_CHECK(formatShowsSeconds(fmt));
  }
  for (const char* fmt : {"", "{:%H:%M}", "%R", "%-I:%M %p", "%x", "%%S", "100%% %H", "%OH:%EM", "%"}) {
    TEST_CHECK(!formatShowsSeconds(fmt));
  }
  return 0;
}
