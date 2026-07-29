#include "system/cpu_stat.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace noctalia::system::cpu_stat {

  namespace {

    // The label is everything before the first space: "cpu" for the aggregate row, "cpuN" for a
    // core. Returns an empty view for a row with no space at all.
    [[nodiscard]] std::string_view labelOf(const std::string& line) {
      const auto space = line.find(' ');
      return space == std::string::npos ? std::string_view{} : std::string_view{line}.substr(0, space);
    }

    [[nodiscard]] bool isCoreLabel(std::string_view label) {
      if (!label.starts_with("cpu") || label.size() == 3) {
        return false;
      }
      const std::string_view index = label.substr(3);
      return std::ranges::all_of(index, [](char c) { return c >= '0' && c <= '9'; });
    }

  } // namespace

  std::optional<Totals> parseLine(const std::string& line, std::string_view expectedLabel) {
    std::istringstream iss{line};
    std::string cpuLabel;
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;

    // user/nice/system/idle are mandatory, so a row with a plausible label but unreadable counters
    // is rejected rather than read as all-zero. The trailing fields are optional: a kernel
    // predating one leaves it zero-initialised rather than poisoning the sum.
    if (!(iss >> cpuLabel >> user >> nice >> system >> idle)) {
      return std::nullopt;
    }
    if (cpuLabel != expectedLabel) {
      return std::nullopt;
    }
    iss >> iowait >> irq >> softirq >> steal;

    Totals totals{};
    totals.idle = idle + iowait;
    totals.total = user + nice + system + idle + iowait + irq + softirq + steal;
    return totals;
  }

  std::optional<double> usageBetween(const Totals& prev, const Totals& current) {
    if (current.total <= prev.total) {
      return std::nullopt;
    }
    const std::uint64_t totalDelta = current.total - prev.total;
    const std::uint64_t idleDelta = current.idle >= prev.idle ? current.idle - prev.idle : 0;
    const double busy = 1.0 - (static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
    return std::clamp(100.0 * busy, 0.0, 100.0);
  }

  std::optional<Totals> readTotals(const std::filesystem::path& statPath) {
    std::ifstream file{statPath};
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::string line;
    if (!std::getline(file, line)) {
      return std::nullopt;
    }

    return parseLine(line, "cpu");
  }

  std::optional<std::vector<Totals>> readCoreTotals(const std::filesystem::path& statPath) {
    std::ifstream file{statPath};
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::vector<Totals> cores;
    std::string line;
    // The cpu rows lead the file, so stop at the first row that is not one rather than scanning
    // the whole of /proc/stat.
    while (std::getline(file, line)) {
      const std::string_view label = labelOf(line);
      if (label == "cpu") {
        continue; // the aggregate row
      }
      if (!isCoreLabel(label)) {
        break;
      }
      // The expected label comes from the row itself: offline cores are omitted from /proc/stat,
      // so predicting "cpu" + cores.size() would desync permanently at the first gap.
      const auto totals = parseLine(line, label);
      if (!totals.has_value()) {
        return std::nullopt;
      }
      cores.push_back(*totals);
    }

    if (cores.empty()) {
      return std::nullopt;
    }
    return cores;
  }

} // namespace noctalia::system::cpu_stat
