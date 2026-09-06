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

  [[nodiscard]] std::string formatBinaryMib(std::uint64_t mib);
  [[nodiscard]] std::string formatBinaryMibAsGib(std::uint64_t mib);
  [[nodiscard]] std::string formatBinaryMibUsageAsGib(std::uint64_t usedMib, std::uint64_t totalMib);
  [[nodiscard]] std::string formatBinaryBytesAsGib(std::uint64_t bytes);
  [[nodiscard]] std::string formatDecimalBytesUsage(double usedBytes, double totalBytes);
  [[nodiscard]] std::string formatDecimalBytesAsGb(double bytes);
  [[nodiscard]] DecimalByteRateUnit decimalByteRateUnitFromString(std::string_view value);
  [[nodiscard]] std::string formatDecimalBytesPerSecond(
      double bytesPerSec, DecimalByteRateUnit unit = DecimalByteRateUnit::Auto,
      ByteRateLabelStyle labelStyle = ByteRateLabelStyle::Full, int compactDecimalPlaces = 1
  );

} // namespace FormatUnits
