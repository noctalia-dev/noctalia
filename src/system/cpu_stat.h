#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::system::cpu_stat {

  // Idle and total jiffies accumulated by one CPU since boot.
  struct Totals {
    std::uint64_t total{0};
    std::uint64_t idle{0};
  };

  // Parses one "/proc/stat" cpu row. `expectedLabel` pins which row is accepted ("cpu" for the
  // aggregate, "cpuN" for a core), so the aggregate cannot be mistaken for core 0. guest and
  // guest_nice are not read: the kernel already folds them into user and nice.
  [[nodiscard]] std::optional<Totals> parseLine(const std::string& line, std::string_view expectedLabel);

  // Busy percentage in [0, 100] between two samples, or nullopt when the window holds no jiffies
  // or the counters went backwards (suspend/resume, container reset).
  [[nodiscard]] std::optional<double> usageBetween(const Totals& prev, const Totals& current);

  // The stat path is a parameter so tests can feed fixtures instead of the live /proc, matching
  // the hwmonRoot/thermalRoot seam in cpu_temp::read().
  [[nodiscard]] std::optional<Totals> readTotals(const std::filesystem::path& statPath = "/proc/stat");

  // Online cores in "/proc/stat" order, skipping the leading aggregate row. Offline cores are
  // absent from the file, so the "cpuN" labels are not necessarily contiguous and an entry's
  // position is not its core id. nullopt when the file is unreadable, holds no cpuN rows, or
  // holds one that does not parse.
  [[nodiscard]] std::optional<std::vector<Totals>> readCoreTotals(const std::filesystem::path& statPath = "/proc/stat");

} // namespace noctalia::system::cpu_stat
