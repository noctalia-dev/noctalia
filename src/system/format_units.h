#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace FormatUnits {

  inline constexpr int kMinCompactByteRateDecimalPlaces = 0;
  inline constexpr int kMaxCompactByteRateDecimalPlaces = 3;

  enum class DecimalByteRateUnit {
    Auto,
    Kilobytes,
    Megabytes,
  };
  enum class ByteRateLabelStyle {
    Full,
    Compact,
  };

  std::string formatBinaryMib(std::uint64_t mib);
  std::string formatBinaryMibAsGib(std::uint64_t mib);
  std::string formatBinaryMibUsageAsGib(std::uint64_t usedMib, std::uint64_t totalMib);
  std::string formatBinaryBytesAsGib(std::uint64_t bytes);
  std::string formatDecimalBytesUsage(double usedBytes, double totalBytes);
  std::string formatDecimalBytesAsGb(double bytes);
  DecimalByteRateUnit decimalByteRateUnitFromString(std::string_view value);
  std::string formatDecimalBytesPerSecond(
      double bytesPerSec, DecimalByteRateUnit unit = DecimalByteRateUnit::Auto,
      ByteRateLabelStyle labelStyle = ByteRateLabelStyle::Full, int compactDecimalPlaces = 1
  );

} // namespace FormatUnits
