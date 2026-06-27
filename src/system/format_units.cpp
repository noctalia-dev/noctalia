#include "system/format_units.h"

#include <format>

namespace FormatUnits {
  namespace {

    constexpr double kMibPerGib = 1024.0;
    constexpr double kBytesPerGib = 1024.0 * 1024.0 * 1024.0;
    constexpr double kBytesPerKb = 1000.0;
    constexpr double kBytesPerMb = 1000.0 * 1000.0;
    constexpr double kBytesPerGb = 1000.0 * 1000.0 * 1000.0;
    constexpr double kBytesPerTb = 1000.0 * 1000.0 * 1000.0 * 1000.0;

    [[nodiscard]] std::string formatByteRateValue(
        double value, std::string_view fullSuffix, std::string_view compactSuffix, ByteRateLabelStyle labelStyle
    ) {
      if (labelStyle == ByteRateLabelStyle::Compact) {
        return std::format("{:.1f}{}", value, compactSuffix);
      }
      return std::format("{:.1f} {}", value, fullSuffix);
    }

    [[nodiscard]] std::string formatByteRateBytes(double bytesPerSec, ByteRateLabelStyle labelStyle) {
      if (labelStyle == ByteRateLabelStyle::Compact) {
        return std::format("{:.0f}B", bytesPerSec);
      }
      return std::format("{:.0f} B/s", bytesPerSec);
    }

  } // namespace

  std::string formatBinaryMib(std::uint64_t mib) {
    if (mib >= static_cast<std::uint64_t>(kMibPerGib)) {
      return formatBinaryMibAsGib(mib);
    }
    return std::format("{} MiB", mib);
  }

  std::string formatBinaryMibAsGib(std::uint64_t mib) {
    return std::format("{:.1f} GiB", static_cast<double>(mib) / kMibPerGib);
  }

  std::string formatBinaryMibUsageAsGib(std::uint64_t usedMib, std::uint64_t totalMib) {
    return std::format(
        "{:.1f} / {:.1f} GiB", static_cast<double>(usedMib) / kMibPerGib, static_cast<double>(totalMib) / kMibPerGib
    );
  }

  std::string formatBinaryBytesAsGib(std::uint64_t bytes) {
    return std::format("{:.1f} GiB", static_cast<double>(bytes) / kBytesPerGib);
  }

  std::string formatDecimalBytesUsage(double usedBytes, double totalBytes) {
    // Pick the unit from the total so both numbers share it; TB once disks pass 1000 GB
    // keeps the column narrow and readable (e.g. "1.3 / 2.0 TB" not "1332.2 / 1967.9 GB").
    if (totalBytes >= kBytesPerTb) {
      return std::format("{:.1f} / {:.1f} TB", usedBytes / kBytesPerTb, totalBytes / kBytesPerTb);
    }
    return std::format("{:.1f} / {:.1f} GB", usedBytes / kBytesPerGb, totalBytes / kBytesPerGb);
  }

  std::string formatDecimalBytesAsGb(double bytes) { return std::format("{:.1f} GB", bytes / kBytesPerGb); }

  DecimalByteRateUnit decimalByteRateUnitFromString(std::string_view value) {
    if (value == "kb") {
      return DecimalByteRateUnit::Kilobytes;
    }
    if (value == "mb") {
      return DecimalByteRateUnit::Megabytes;
    }
    return DecimalByteRateUnit::Auto;
  }

  std::string formatDecimalBytesPerSecond(double bytesPerSec, DecimalByteRateUnit unit, ByteRateLabelStyle labelStyle) {
    switch (unit) {
    case DecimalByteRateUnit::Kilobytes:
      return formatByteRateValue(bytesPerSec / kBytesPerKb, "kB/s", "k", labelStyle);
    case DecimalByteRateUnit::Megabytes:
      return formatByteRateValue(bytesPerSec / kBytesPerMb, "MB/s", "M", labelStyle);
    case DecimalByteRateUnit::Auto:
      break;
    }

    if (bytesPerSec >= kBytesPerGb) {
      return formatByteRateValue(bytesPerSec / kBytesPerGb, "GB/s", "G", labelStyle);
    }
    if (bytesPerSec >= kBytesPerMb) {
      return formatByteRateValue(bytesPerSec / kBytesPerMb, "MB/s", "M", labelStyle);
    }
    if (bytesPerSec >= kBytesPerKb) {
      return formatByteRateValue(bytesPerSec / kBytesPerKb, "kB/s", "k", labelStyle);
    }
    return formatByteRateBytes(bytesPerSec, labelStyle);
  }

} // namespace FormatUnits
