#include "i18n/i18n_service.h"
#include "time/time_format.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <print>
#include <string>
#include <string_view>

namespace {

  bool expectEqual(std::string_view actual, std::string_view expected, const char* message) {
    if (actual != expected) {
      std::println(stderr, "time_format_test: {}: expected '{}', got '{}'", message, expected, actual);
      return false;
    }
    return true;
  }

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

  bool ok = true;
  ok = expectEqual(formatLocalUnixTime(1700000000, ""), "", "empty unix format is safe") && ok;
  ok = expectEqual(formatLocalTime(""), "", "empty local format is safe") && ok;
  ok = expectEqual(formatLocalTime(nullptr), "", "null local format is safe") && ok;
  ok = expectEqual(formatTimezoneTime("", "UTC"), "", "empty timezone format is safe") && ok;
  ok = expectEqual(formatLocalUnixTime(1700000000, "%s"), "1700000000", "formats unix epoch token") && ok;
  ok = expectEqual(
           formatLocalUnixTime(1700000000, "recording_%s"), "recording_1700000000",
           "formats epoch inside filename pattern"
       )
      && ok;
  ok = expectEqual(formatLocalUnixTime(1700000000, "%%s_%s"), "%s_1700000000", "keeps escaped percent literal") && ok;
  ok = expectEqual(isValidTimezone("") ? "valid" : "invalid", "valid", "accepts empty system-local timezone") && ok;
  ok = expectEqual(isValidTimezone("UTC") ? "valid" : "invalid", "valid", "accepts known timezone") && ok;
  ok = expectEqual(
           isValidTimezone("Europe/Berln") ? "valid" : "invalid", "invalid", "rejects unknown non-empty timezone"
       )
      && ok;
  const auto beforeTimezoneFormat = floor<seconds>(system_clock::now());
  const auto* kiritimati = locate_zone("Pacific/Kiritimati");
  ok = expectEqual(
           formatTimezoneTime("%z|%Z", kiritimati->name()), expectedZoneLabel(beforeTimezoneFormat, *kiritimati),
           "formats configured timezone offset and abbreviation"
       )
      && ok;
  ok = expectEqual(
           formatTimezoneTime("{:%Z}", kiritimati->name()), formatTimezoneTime("%Z", kiritimati->name()),
           "renders the zone abbreviation the same in a chrono field as in a bare spec"
       )
      && ok;
  const std::string formattedUtcDay = formatTimezoneTime("%j", "UTC");
  const auto afterTimezoneFormat = floor<seconds>(system_clock::now());
  const bool utcDayMatches =
      formattedUtcDay == utcDayOfYear(beforeTimezoneFormat) || formattedUtcDay == utcDayOfYear(afterTimezoneFormat);
  ok = expectEqual(utcDayMatches ? "match" : formattedUtcDay, "match", "formats configured timezone day of year") && ok;
  ok = expectEqual(formatDuration(59s), "<1m", "formats sub-minute duration") && ok;
  ok = expectEqual(formatDuration(1min), "1 minute", "formats singular minute") && ok;
  ok = expectEqual(formatDuration(2h + 1min), "2 hours 1 minute", "formats hours and minutes") && ok;
  ok = expectEqual(formatDuration(24h + 1h + 1min), "1 day 1 hour 1 minute", "formats days hours and minutes") && ok;

  // Both rendering paths must report the same local time: a %-spec goes through strftime, a bare
  // {} through std::format. Deriving the offset from one clock keeps them on the configured zone.
  ::setenv("TZ", "Asia/Kathmandu", 1);
  ::tzset();
  constexpr std::int64_t kFixedStamp = 1700000000; // 2023-11-14T22:13:20Z, +05:45 in Kathmandu.
  ok = expectEqual(
           formatLocalUnixTime(kFixedStamp, "%Y-%m-%d %H:%M:%S"), "2023-11-15 03:58:20",
           "strftime path honors the configured timezone"
       )
      && ok;
  ok = expectEqual(
           formatLocalUnixTime(kFixedStamp, "{}"), "2023-11-15 03:58:20", "chrono path honors the configured timezone"
       )
      && ok;
  return ok ? 0 : 1;
}
