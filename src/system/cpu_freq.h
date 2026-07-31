#pragma once

#include <filesystem>
#include <optional>

namespace noctalia::system::cpu_freq {

  // Current per-core clock in MHz, averaged across the cores that report one; scaling_cur_freq is in kHz.
  // Zero-valued reads (drivers report 0 during idle transitions) are skipped; nullopt when nothing
  // readable exists (no cpufreq driver). `root` is a seam for tests.
  [[nodiscard]] std::optional<double> readCurFreqMhz(const std::filesystem::path& root = "/sys/devices/system/cpu");

  // Highest core clock ceiling in MHz: scaling_max_freq per core, falling back to cpuinfo_max_freq.
  // Used only to normalize graph/gauge values. nullopt when the system exposes neither.
  [[nodiscard]] std::optional<double> readMaxFreqMhz(const std::filesystem::path& root = "/sys/devices/system/cpu");

} // namespace noctalia::system::cpu_freq
