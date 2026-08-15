#include "system/cpu_freq.h"

#include "util/file_utils.h"
#include "util/sys_utils.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <optional>
#include <system_error>

namespace noctalia::system::cpu_freq {

  namespace {
    // Parse a kHz/mHz integer file, or nullopt on absence/garbage.
    [[nodiscard]] std::optional<std::uint64_t> readUintFile(const std::filesystem::path& path) {
      const auto text = FileUtils::readSmallTextFile(path);
      if (!text.has_value())
        return std::nullopt;
      std::uint64_t value = 0;
      const auto [ptr, ec] = std::from_chars(text->data(), text->data() + text->size(), value);
      if (ec != std::errc{} || ptr != text->data() + text->size())
        return std::nullopt;
      return value;
    }

  } // namespace

  CpuFreqs readFreqs(const std::filesystem::path& root) {
    CpuFreqs freqs;
    double totalMhz = 0.0;
    std::size_t count = 0;
    std::uint64_t best = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{root, ec}) {
      if (!entry.is_directory(ec) || !SysUtils::isCpuN(entry.path().filename().string()))
        continue;
      const auto base = entry.path() / "cpufreq";
      if (const auto kHz = readUintFile(base / "scaling_cur_freq"); kHz.has_value() && *kHz != 0) {
        totalMhz += static_cast<double>(*kHz) / 1000.0;
        ++count;
      }
      if (const auto maxKhz = readUintFile(base / "scaling_max_freq"); maxKhz.has_value())
        best = std::max(best, *maxKhz);
    }
    if (count > 0)
      freqs.curMhz = totalMhz / static_cast<double>(count);
    if (best > 0)
      freqs.maxMhz = static_cast<double>(best) / 1000.0;
    return freqs;
  }

} // namespace noctalia::system::cpu_freq