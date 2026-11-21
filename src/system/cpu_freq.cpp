#include "system/cpu_freq.h"

#include "util/sys_utils.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace noctalia::system::cpu_freq {

  namespace {

    enum class ReadStatus { Ok, Missing, Failed };

    // Parses a kHz integer from an open sysfs attribute; sysfs regenerates it on every read from offset 0.
    ReadStatus readUintFd(int fd, std::uint64_t& value) {
      if (fd < 0) {
        return ReadStatus::Missing;
      }
      char buf[32];
      const ssize_t n = ::pread(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        return ReadStatus::Failed;
      }
      std::string_view text(buf, static_cast<std::size_t>(n));
      while (!text.empty()
             && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
      }
      if (text.empty()) {
        return ReadStatus::Failed;
      }
      const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return ReadStatus::Failed;
      }
      return ReadStatus::Ok;
    }

    int openReadOnly(const std::filesystem::path& path) { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC); }

  } // namespace

  Reader::Reader(std::filesystem::path root) : m_root(std::move(root)) {}

  Reader::~Reader() { closeAll(); }

  void Reader::closeAll() noexcept {
    for (const Core& core : m_cores) {
      if (core.curFd >= 0) {
        ::close(core.curFd);
      }
      if (core.maxFd >= 0) {
        ::close(core.maxFd);
      }
    }
    m_cores.clear();
  }

  void Reader::scan() {
    closeAll();
    m_scanned = true;
    m_readsSinceScan = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{m_root, ec}) {
      if (!entry.is_directory(ec) || !SysUtils::isCpuN(entry.path().filename().string())) {
        continue;
      }
      const auto base = entry.path() / "cpufreq";
      Core core{
          .curFd = openReadOnly(base / "scaling_cur_freq"),
          .maxFd = openReadOnly(base / "scaling_max_freq"),
      };
      if (core.curFd >= 0 || core.maxFd >= 0) {
        m_cores.push_back(core);
      }
    }
  }

  CpuFreqs Reader::read() {
    if (!m_scanned || m_readsSinceScan >= kRescanInterval) {
      scan();
    }
    ++m_readsSinceScan;

    CpuFreqs freqs;
    double totalMhz = 0.0;
    std::size_t count = 0;
    std::uint64_t best = 0;
    bool failed = false;
    for (const Core& core : m_cores) {
      std::uint64_t kHz = 0;
      switch (readUintFd(core.curFd, kHz)) {
      case ReadStatus::Ok:
        if (kHz != 0) {
          totalMhz += static_cast<double>(kHz) / 1000.0;
          ++count;
        }
        break;
      case ReadStatus::Failed:
        failed = true;
        break;
      case ReadStatus::Missing:
        break;
      }
      std::uint64_t maxKhz = 0;
      switch (readUintFd(core.maxFd, maxKhz)) {
      case ReadStatus::Ok:
        best = std::max(best, maxKhz);
        break;
      case ReadStatus::Failed:
        failed = true;
        break;
      case ReadStatus::Missing:
        break;
      }
    }
    if (failed) {
      // A core went offline or its cpufreq policy changed; rescan before the next read.
      m_scanned = false;
    }
    if (count > 0) {
      freqs.curMhz = totalMhz / static_cast<double>(count);
    }
    if (best > 0) {
      freqs.maxMhz = static_cast<double>(best) / 1000.0;
    }
    return freqs;
  }

  CpuFreqs readFreqs(const std::filesystem::path& root) { return Reader(root).read(); }

} // namespace noctalia::system::cpu_freq
