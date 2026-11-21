#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace noctalia::system::cpu_freq {

  // curMhz: average clock across cores (scaling_cur_freq, kHz; zero reads skipped).
  // maxMhz: highest scaling_max_freq ceiling seen; normalizes graph/gauge values.
  // Both nullopt when nothing readable exists (no cpufreq driver). `root` is a test seam.
  struct CpuFreqs {
    std::optional<double> curMhz;
    std::optional<double> maxMhz;
  };

  // Keeps every core's cpufreq files open and re-reads them with pread(). The directory is scanned
  // again after a failed read and every kRescanInterval reads, so hotplugged cores are picked up.
  class Reader {
  public:
    explicit Reader(std::filesystem::path root = "/sys/devices/system/cpu");
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    CpuFreqs read();

  private:
    static constexpr int kRescanInterval = 60;

    struct Core {
      int curFd = -1;
      int maxFd = -1;
    };

    void scan();
    void closeAll() noexcept;

    std::filesystem::path m_root;
    std::vector<Core> m_cores;
    bool m_scanned = false;
    int m_readsSinceScan = 0;
  };

  CpuFreqs readFreqs(const std::filesystem::path& root = "/sys/devices/system/cpu");

} // namespace noctalia::system::cpu_freq
