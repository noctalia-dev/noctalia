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

  std::string formatDecimalBytesPerSecond(double bytesPerSec, DecimalByteRateUnit unit) {
    switch (unit) {
    case DecimalByteRateUnit::Kilobytes:
      return std::format("{:.1f} kB/s", bytesPerSec / kBytesPerKb);
    case DecimalByteRateUnit::Megabytes:
      return std::format("{:.1f} MB/s", bytesPerSec / kBytesPerMb);
    case DecimalByteRateUnit::Auto:
      break;
    }

    if (bytesPerSec >= kBytesPerGb) {
      return std::format("{:.1f} GB/s", bytesPerSec / kBytesPerGb);
    }
    if (bytesPerSec >= kBytesPerMb) {
      return std::format("{:.1f} MB/s", bytesPerSec / kBytesPerMb);
    }
    if (bytesPerSec >= kBytesPerKb) {
      return std::format("{:.1f} kB/s", bytesPerSec / kBytesPerKb);
    }
    return std::format("{:.0f} B/s", bytesPerSec);
  }

} // namespace FormatUnits
