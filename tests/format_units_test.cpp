#include "system/format_units.h"
#include "tests/test_check.h"

#include <string_view>

int main() {
  using FormatUnits::ByteRateLabelStyle;
  using FormatUnits::DecimalByteRateUnit;

  TEST_CHECK(
      FormatUnits::formatDecimalBytesPerSecond(20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 0)
      == std::string_view{"20k"}
  );
  TEST_CHECK(
      FormatUnits::formatDecimalBytesPerSecond(20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 1)
      == std::string_view{"20.3k"}
  );
  TEST_CHECK(
      FormatUnits::formatDecimalBytesPerSecond(20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 3)
      == std::string_view{"20.300k"}
  );
  // Clamps compact precision below the supported range.
  TEST_CHECK(
      FormatUnits::formatDecimalBytesPerSecond(20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, -1)
      == std::string_view{"20k"}
  );
  // Clamps compact precision above the supported range.
  TEST_CHECK(
      FormatUnits::formatDecimalBytesPerSecond(20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 4)
      == std::string_view{"20.300k"}
  );
  TEST_CHECK(
      FormatUnits::formatDecimalBytesPerSecond(20600.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Compact, 0)
      == std::string_view{"21k"}
  );
  TEST_CHECK(
      FormatUnits::formatDecimalBytesPerSecond(
          2048000.0, DecimalByteRateUnit::Megabytes, ByteRateLabelStyle::Compact, 2
      )
      == std::string_view{"2.05M"}
  );
  // Byte values stay whole.
  TEST_CHECK(
      FormatUnits::formatDecimalBytesPerSecond(42.0, DecimalByteRateUnit::Auto, ByteRateLabelStyle::Compact, 3)
      == std::string_view{"42B"}
  );
  // Full network speeds keep their existing precision.
  TEST_CHECK(
      FormatUnits::formatDecimalBytesPerSecond(20300.0, DecimalByteRateUnit::Kilobytes, ByteRateLabelStyle::Full, 3)
      == std::string_view{"20.3 kB/s"}
  );
  return 0;
}
