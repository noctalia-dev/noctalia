#pragma once

#include <filesystem>
#include <optional>

namespace noctalia::system::cpu_freq {

  // curMhz: average clock across cores (scaling_cur_freq, kHz; zero reads skipped).
  // maxMhz: highest scaling_max_freq ceiling seen; normalizes graph/gauge values.
  // Both nullopt when nothing readable exists (no cpufreq driver). `root` is a test seam.
  struct CpuFreqs {
    std::optional<double> curMhz;
    std::optional<double> maxMhz;
  };

  [[nodiscard]] CpuFreqs readFreqs(const std::filesystem::path& root = "/sys/devices/system/cpu");

} // namespace noctalia::system::cpu_freq