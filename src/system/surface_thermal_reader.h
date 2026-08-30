#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace noctalia::system::surface {

  struct FanReading {
    std::uint32_t rpm = 0;
  };

  struct TempReading {
    std::string label;
    double celsius = 0.0;
  };

  struct ThermalSnapshot {
    std::optional<FanReading> fan;
    std::vector<TempReading> temperatures;
  };

  /// Read Surface fan RPM and thermal sensors from hwmon. Injectable root for tests.
  [[nodiscard]] ThermalSnapshot readThermal(const std::filesystem::path& hwmonRoot = "/sys/class/hwmon");

} // namespace noctalia::system::surface
