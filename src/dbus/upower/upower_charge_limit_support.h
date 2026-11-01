#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

// Internal parsing surface shared with focused tests. This is separate from UPowerService's public
// device model because callers should consume UPowerChargeLimitState, not probe sysfs themselves.
namespace upower::detail {

  struct ChargeThresholdProbe {
    std::optional<std::uint32_t> start;
    std::optional<std::uint32_t> end;

    bool operator==(const ChargeThresholdProbe&) const = default;
  };

  [[nodiscard]] ChargeThresholdProbe readChargeThresholdsFromSysfs(
      std::string_view nativePath, const std::filesystem::path& powerSupplyRoot = "/sys/class/power_supply"
  );
} // namespace upower::detail
