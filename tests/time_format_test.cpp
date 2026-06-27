#include "time/time_format.h"

#include "i18n/i18n_service.h"

#include <chrono>
#include <cstdio>
#include <string_view>

namespace {

  bool expectEqual(std::string_view actual, std::string_view expected, const char* message) {
    if (actual != expected) {
      std::fprintf(
          stderr, "time_format_test: %s: expected '%.*s', got '%.*s'\n", message, static_cast<int>(expected.size()),
          expected.data(), static_cast<int>(actual.size()), actual.data()
      );
      return false;
    }
    return true;
  }

} // namespace

int main() {
  using namespace std::chrono;

  i18n::Service::instance().init("en");

  bool ok = true;
  ok = expectEqual(formatLocalUnixTime(1700000000, "%s"), "1700000000", "formats unix epoch token") && ok;
  ok = expectEqual(
           formatLocalUnixTime(1700000000, "recording_%s"), "recording_1700000000",
           "formats epoch inside filename pattern"
       )
      && ok;
  ok = expectEqual(formatLocalUnixTime(1700000000, "%%s_%s"), "%s_1700000000", "keeps escaped percent literal") && ok;
  ok = expectEqual(formatDuration(59s), "<1m", "formats sub-minute duration") && ok;
  ok = expectEqual(formatDuration(1min), "1 minute", "formats singular minute") && ok;
  ok = expectEqual(formatDuration(2h + 1min), "2 hours 1 minute", "formats hours and minutes") && ok;
  ok = expectEqual(formatDuration(24h + 1h + 1min), "1 day 1 hour 1 minute", "formats days hours and minutes")
      && ok;
  return ok ? 0 : 1;
}
