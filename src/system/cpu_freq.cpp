#include "system/cpu_freq.h"

#include "util/file_utils.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace noctalia::system::cpu_freq {

  namespace {
    // cpu0..cpuN are directories; anything else under the root (cpufreq/, offline, ...) is skipped.
    [[nodiscard]] bool isCpuDir(const std::filesystem::path& dir) {
      const std::string name = dir.filename().string();
      if (!name.starts_with("cpu") || name.size() == 3) {
        return false;
      }
      return std::all_of(name.begin() + 3, name.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    }

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

  std::optional<double> readCurFreqMhz(const std::filesystem::path& root) {
    double totalMhz = 0.0;
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{root, ec}) {
      if (!entry.is_directory(ec) || !isCpuDir(entry.path().filename()))
        continue;
      const auto path = entry.path() / "cpufreq" / "scaling_cur_freq";
      const auto kHz = readUintFile(path);
      if (!kHz.has_value() || *kHz == 0)
        continue; // zero = driver transitional state
      totalMhz += static_cast<double>(*kHz) / 1000.0;
      ++count;
    }
    if (count == 0)
      return std::nullopt;
    return totalMhz / static_cast<double>(count);
  }

  std::optional<double> readMaxFreqMhz(const std::filesystem::path& root) {
    std::uint64_t best = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{root, ec}) {
      if (!entry.is_directory(ec) || !isCpuDir(entry.path().filename()))
        continue;
      const auto base = entry.path() / "cpufreq";
      auto value = readUintFile(base / "scaling_max_freq");
      if (!value.has_value())
        value = readUintFile(entry.path() / "cpuinfo_max_freq");
      if (value.has_value())
        best = std::max(best, *value);
    }
    return best > 0 ? std::optional<double>{static_cast<double>(best) / 1000.0} : std::nullopt;
  }

} // namespace noctalia::system::cpu_freq