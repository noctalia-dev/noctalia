#include "system/format_units.h"

#include <cstdio>
#include <print>
#include <string_view>

namespace {

  bool expectEqual(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
      std::println(stderr, "format_units_test: {}: expected '{}', got '{}'", message, expected, actual);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  using FormatUnits::ByteRateLabelStyle;
  using FormatUnits::DecimalByteRateUnit;

  bool ok = true;
  ok = expectEqual(
           FormatUnits::formatDecimalBytesPerSecond(
               20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 0
           ),
           "20k", "rounds compact kilobytes to whole numbers"
       )
      && ok;
  ok = expectEqual(
           FormatUnits::formatDecimalBytesPerSecond(
               20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 1
           ),
           "20.3k", "formats the default compact precision"
       )
      && ok;
  ok = expectEqual(
           FormatUnits::formatDecimalBytesPerSecond(
               20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 3
           ),
           "20.300k", "formats a higher compact precision"
       )
      && ok;
  ok = expectEqual(
           FormatUnits::formatDecimalBytesPerSecond(
               20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, -1
           ),
           "20k", "clamps compact precision below the supported range"
       )
      && ok;
  ok = expectEqual(
           FormatUnits::formatDecimalBytesPerSecond(
               20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 4
           ),
           "20.300k", "clamps compact precision above the supported range"
       )
      && ok;
  ok = expectEqual(
           FormatUnits::formatDecimalBytesPerSecond(
               20600.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 0
           ),
           "21k", "rounds compact kilobytes"
       )
      && ok;
  ok =
      expectEqual(
          FormatUnits::formatDecimalBytesPerSecond(999500.0, DecimalByteRateUnit::Auto, ByteRateLabelStyle::Compact, 0),
          "1000k", "rounds at a compact unit boundary without changing units"
      )
      && ok;
  ok = expectEqual(
           FormatUnits::formatDecimalBytesPerSecond(
               2048000.0, DecimalByteRateUnit::Megabytes, ByteRateLabelStyle::Compact, 2
           ),
           "2.05M", "formats megabytes with configured precision"
       )
      && ok;
  ok = expectEqual(
           FormatUnits::formatDecimalBytesPerSecond(42.0, DecimalByteRateUnit::Auto, ByteRateLabelStyle::Compact, 3),
           "42B", "keeps byte values whole"
       )
      && ok;
  ok = expectEqual(
           FormatUnits::formatDecimalBytesPerSecond(
               20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Full, 3
           ),
           "20.3 kB/s", "keeps full network speeds at their existing precision"
       )
      && ok;
  return ok ? 0 : 1;
}
