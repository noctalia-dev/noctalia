#include "system/system_monitor_service.h"

#include "core/log.h"
#include "system/cpu_temp_sensor.h"
#include "system/format_units.h"
#include "system/intel_gpu.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <unordered_map>
#include <vector>

namespace {

  [[nodiscard]] SystemStats makeInitialHistoryStats() {
    SystemStats stats;
    stats.cpuTempC = 40.0;
    return stats;
  }

  template <typename T, std::size_t N>
  [[nodiscard]] std::vector<T> historyWindowFromRing(const std::array<T, N>& ring, int head, int windowSize) {
    if (windowSize <= 0) {
      return {};
    }

    const int ringSize = static_cast<int>(N);
    const int clampedWindow = std::min(windowSize, ringSize);
    std::vector<T> result;
    result.reserve(static_cast<std::size_t>(clampedWindow));
    const int start = (head - clampedWindow + ringSize) % ringSize;
    for (int i = 0; i < clampedWindow; ++i) {
      const int idx = (start + i) % ringSize;
      result.push_back(ring[static_cast<std::size_t>(idx)]);
    }
    return result;
  }

  std::optional<double> readTempInputCelsius(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file.is_open()) {
      return std::nullopt;
    }

    long long raw = 0;
    file >> raw;
    if (file.fail() || raw <= 0) {
      return std::nullopt;
    }

    // Most Linux temp files are millidegrees Celsius.
    if (raw >= 1000) {
      return static_cast<double>(raw) / 1000.0;
    }
    return static_cast<double>(raw);
  }

  std::optional<std::uint64_t> readUint64File(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::uint64_t value = 0;
    file >> value;
    if (file.fail()) {
      return std::nullopt;
    }
    return value;
  }

  struct TempSensorReading {
    double tempC = 0.0;
    int score = -1;
    std::string source;
    bool isNvidia = false;
  };

  struct GpuHwmonProbe {
    std::optional<TempSensorReading> reading;
    bool foundNvidia = false;
  };

  struct GpuVramReading {
    std::uint64_t usedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::string source;
    bool isNvidia = false;
  };

  struct AmdGpuSysfsDevice {
    std::filesystem::path devicePath;
    std::filesystem::path hwmonPath;
    bool hasBusy = false;
    bool hasTemp = false;
    bool hasVram = false;
  };

  [[nodiscard]] bool hasUsableVram(const GpuVramReading& reading) {
    return reading.totalBytes > 0 && reading.usedBytes <= reading.totalBytes;
  }

  // 0 disables a metric; any other value is clamped to the supported poll range.
  [[nodiscard]] float clampPollSeconds(float seconds) noexcept {
    if (seconds <= 0.0f) {
      return SystemConfig::MonitorConfig::kDisabledPollSeconds;
    }
    return std::clamp(
        seconds, SystemConfig::MonitorConfig::kMinPollSeconds, SystemConfig::MonitorConfig::kMaxPollSeconds
    );
  }

  // Graph history snapshots and scroll timing follow the fastest enabled metric poll so users
  // only configure how often each stat is read, not a separate graph-only cadence. Disabled
  // metrics (0) are ignored; if every metric is disabled the history is disabled too.
  [[nodiscard]] float effectiveHistoryPollSeconds(const SystemConfig::MonitorConfig& config) noexcept {
    float fastest = SystemConfig::MonitorConfig::kDisabledPollSeconds;
    for (const float seconds :
         {config.cpuPollSeconds, config.gpuPollSeconds, config.memoryPollSeconds, config.networkPollSeconds,
          config.diskPollSeconds}) {
      if (seconds <= 0.0f) {
        continue;
      }
      if (fastest <= 0.0f || seconds < fastest) {
        fastest = seconds;
      }
    }
    return fastest;
  }

  [[nodiscard]] double netRxFromStats(const SystemStats& stats, std::string_view interfaceName) {
    if (interfaceName.empty()) {
      return stats.netRxBytesPerSec;
    }
    if (const auto it = stats.netThroughputByInterface.find(std::string(interfaceName));
        it != stats.netThroughputByInterface.end()) {
      return it->second.rxBytesPerSec;
    }
    return 0.0;
  }

  [[nodiscard]] double netTxFromStats(const SystemStats& stats, std::string_view interfaceName) {
    if (interfaceName.empty()) {
      return stats.netTxBytesPerSec;
    }
    if (const auto it = stats.netThroughputByInterface.find(std::string(interfaceName));
        it != stats.netThroughputByInterface.end()) {
      return it->second.txBytesPerSec;
    }
    return 0.0;
  }

  [[nodiscard]] SystemConfig::MonitorConfig sanitizeMonitorConfig(SystemConfig::MonitorConfig config) {
    config.cpuPollSeconds = clampPollSeconds(config.cpuPollSeconds);
    config.gpuPollSeconds = clampPollSeconds(config.gpuPollSeconds);
    config.memoryPollSeconds = clampPollSeconds(config.memoryPollSeconds);
    config.networkPollSeconds = clampPollSeconds(config.networkPollSeconds);
    config.diskPollSeconds = clampPollSeconds(config.diskPollSeconds);
    return config;
  }

  [[nodiscard]] std::chrono::steady_clock::duration pollDuration(float seconds) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
  }

  void mergeGpuVram(GpuVramReading& target, const GpuVramReading& source) {
    if (!hasUsableVram(source)) {
      return;
    }
    target.usedBytes += source.usedBytes;
    target.totalBytes += source.totalBytes;
    if (target.source.empty()) {
      target.source = source.source;
    } else if (!source.source.empty()) {
      target.source += " + " + source.source;
    }
    target.isNvidia = target.isNvidia || source.isNvidia;
  }

  std::string formatHwmonTempSource(
      const std::string& hwmonName, const std::string& label, const std::filesystem::path& inputPath
  ) {
    const std::string name = hwmonName.empty() ? "unknown" : hwmonName;
    if (label.empty()) {
      return std::format("hwmon:{} {}", name, inputPath.string());
    }
    return std::format("hwmon:{} label=\"{}\" {}", name, label, inputPath.string());
  }

  bool isBetterHwmonSensor(int score, double tempC, int bestScore, const std::optional<double>& bestTemp) {
    return score > bestScore || (score == bestScore && (!bestTemp.has_value() || tempC > *bestTemp));
  }

  int scoreGpuHwmonSensor(const std::string& hwmonName, const std::string& label) {
    const std::string name = StringUtils::toLower(hwmonName);
    const std::string lbl = StringUtils::toLower(label);

    int score = 0;
    if (name == "amdgpu") {
      score += 20;
    } else if (name == "nvidia" || name.contains("nvidia")) {
      score += 20;
    } else if (name == "i915" || name == "xe") {
      score += 20;
    } else if (name == "nouveau") {
      score += 10;
    } else {
      return -1;
    }

    if (lbl.contains("junction") || lbl.contains("edge")) {
      score += 30;
    } else if (lbl.contains("gpu") || lbl.contains("mem")) {
      score += 25;
    }

    return score;
  }

  bool isGpuHwmonAwake(const std::filesystem::path& hwmonPath) {
    namespace fs = std::filesystem;
    const auto deviceLink = hwmonPath / "device";
    if (!fs::exists(deviceLink)) {
      return true;
    }
    const auto status = FileUtils::readSmallTextFile(deviceLink / "power" / "runtime_status");
    if (!status.has_value()) {
      return true;
    }
    return *status == "active";
  }

  bool isDrmCardName(const std::string& name) {
    if (!name.starts_with("card") || name.size() == 4) {
      return false;
    }
    return std::all_of(name.begin() + 4, name.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
  }

  std::filesystem::path findAmdGpuHwmonPath(const std::filesystem::path& devicePath) {
    namespace fs = std::filesystem;

    const fs::path hwmonDir = devicePath / "hwmon";
    std::error_code ec;
    if (!fs::is_directory(hwmonDir, ec)) {
      return {};
    }

    for (const auto& entry : fs::directory_iterator{hwmonDir, ec}) {
      if (entry.is_directory()) {
        return entry.path();
      }
    }
    return {};
  }

  std::vector<AmdGpuSysfsDevice> findAmdGpuSysfsDevices() {
    namespace fs = std::filesystem;

    std::vector<AmdGpuSysfsDevice> devices;
    const fs::path drmRoot{"/sys/class/drm"};
    std::error_code ec;
    if (!fs::is_directory(drmRoot, ec)) {
      return devices;
    }

    for (const auto& entry : fs::directory_iterator{drmRoot, ec}) {
      if (!entry.is_directory() || !isDrmCardName(entry.path().filename().string())) {
        continue;
      }

      const fs::path devicePath = entry.path() / "device";
      if (!fs::exists(devicePath)) {
        continue;
      }

      if (FileUtils::readSmallTextFile(devicePath / "vendor").value_or("") != "0x1002") {
        continue;
      }

      const fs::path driverLink = fs::read_symlink(devicePath / "driver", ec);
      if (ec || driverLink.filename().string() != "amdgpu") {
        continue;
      }

      AmdGpuSysfsDevice device;
      device.devicePath = devicePath;
      device.hwmonPath = findAmdGpuHwmonPath(devicePath);
      device.hasBusy = fs::exists(devicePath / "gpu_busy_percent");
      device.hasVram = fs::exists(devicePath / "mem_info_vram_total");
      if (!device.hwmonPath.empty()) {
        device.hasTemp = fs::exists(device.hwmonPath / "temp1_input");
      }

      if (device.hasBusy || device.hasVram || device.hasTemp) {
        devices.push_back(std::move(device));
      }
    }

    return devices;
  }

  struct SysfsGpuUsageReading {
    double percent = 0.0;
    std::string source;
  };

  std::optional<SysfsGpuUsageReading> readAmdGpuSysfsUsage() {
    for (const auto& device : findAmdGpuSysfsDevices()) {
      if (!device.hasBusy) {
        continue;
      }
      const std::filesystem::path busyPath = device.devicePath / "gpu_busy_percent";
      const auto value = readUint64File(busyPath);
      if (!value.has_value()) {
        continue;
      }

      return SysfsGpuUsageReading{
          .percent = static_cast<double>(std::clamp<std::uint64_t>(*value, 0, 100)),
          .source = std::format("amdgpu sysfs:{}", busyPath.string()),
      };
    }

    return std::nullopt;
  }

  std::optional<TempSensorReading> readAmdGpuSysfsTempSensor() {
    for (const auto& device : findAmdGpuSysfsDevices()) {
      if (!device.hasTemp) {
        continue;
      }

      const std::filesystem::path tempPath = device.hwmonPath / "temp1_input";
      const auto tempC = readTempInputCelsius(tempPath);
      if (!tempC.has_value()) {
        continue;
      }

      return TempSensorReading{
          .tempC = *tempC, .score = 0, .source = std::format("amdgpu sysfs:{}", tempPath.string())
      };
    }

    return std::nullopt;
  }

  std::optional<GpuVramReading> readAmdGpuVram() {
    GpuVramReading total;
    int deviceCount = 0;
    std::string firstSource;

    for (const auto& device : findAmdGpuSysfsDevices()) {
      if (!device.hasVram) {
        continue;
      }

      const std::filesystem::path usedPath = device.devicePath / "mem_info_vram_used";
      const std::filesystem::path totalPath = device.devicePath / "mem_info_vram_total";
      const auto used = readUint64File(usedPath);
      const auto available = readUint64File(totalPath);
      if (!used.has_value() || !available.has_value() || *available == 0 || *used > *available) {
        continue;
      }

      ++deviceCount;
      if (firstSource.empty()) {
        firstSource = usedPath.string();
      }
      mergeGpuVram(
          total, GpuVramReading{.usedBytes = *used, .totalBytes = *available, .source = {}, .isNvidia = false}
      );
    }

    if (deviceCount <= 0 || !hasUsableVram(total)) {
      return std::nullopt;
    }

    total.source = deviceCount == 1 ? std::format("amdgpu:{}", firstSource)
                                    : std::format("amdgpu sysfs ({} devices)", deviceCount);
    return total;
  }

  noctalia::system::cpu_temp::ProbeResult readCpuTempSensor(const SystemConfig::MonitorConfig& config) {
    try {
      return noctalia::system::cpu_temp::read("/sys/class/hwmon", "/sys/class/thermal", config.cpuTempSensorPath);
    } catch (...) {
      return noctalia::system::cpu_temp::ProbeResult{
          .reading = std::nullopt, .error = "CPU temperature sensor scan failed"
      };
    }
  }

  GpuHwmonProbe readGpuHwmonTempSensor() {
    namespace fs = std::filesystem;

    GpuHwmonProbe probe;
    const fs::path hwmonRoot{"/sys/class/hwmon"};
    if (!fs::exists(hwmonRoot) || !fs::is_directory(hwmonRoot)) {
      return probe;
    }

    int bestScore = -1;
    for (const auto& hwmonEntry : fs::directory_iterator{hwmonRoot}) {
      if (!hwmonEntry.is_directory()) {
        continue;
      }

      const std::string hwmonName = FileUtils::readSmallTextFile(hwmonEntry.path() / "name").value_or("");
      const int nameScore = scoreGpuHwmonSensor(hwmonName, "");
      if (nameScore < 0) {
        continue;
      }

      const std::string normalizedName = StringUtils::toLower(hwmonName);
      const bool isNvidia = normalizedName == "nvidia" || normalizedName.contains("nvidia");

      if (!isGpuHwmonAwake(hwmonEntry.path())) {
        continue;
      }

      for (const auto& fileEntry : fs::directory_iterator{hwmonEntry.path()}) {
        if (!fileEntry.is_regular_file()) {
          continue;
        }

        const std::string fileName = fileEntry.path().filename().string();
        if (!fileName.starts_with("temp") || !fileName.ends_with("_input")) {
          continue;
        }

        const std::string base = fileName.substr(0, fileName.size() - 6);
        const std::string label = FileUtils::readSmallTextFile(hwmonEntry.path() / (base + "_label")).value_or("");
        const auto tempC = readTempInputCelsius(fileEntry.path());
        if (!tempC.has_value()) {
          continue;
        }

        const int score = scoreGpuHwmonSensor(hwmonName, label);
        if (isNvidia) {
          probe.foundNvidia = true;
        }
        if (isBetterHwmonSensor(
                score, *tempC, bestScore,
                probe.reading.has_value() ? std::optional<double>{probe.reading->tempC} : std::nullopt
            )) {
          bestScore = score;
          probe.reading = TempSensorReading{
              .tempC = *tempC,
              .score = score,
              .source = formatHwmonTempSource(hwmonName, label, fileEntry.path()),
              .isNvidia = isNvidia
          };
        }
      }
    }

    return probe;
  }

  bool isInactiveRuntimeStatus(const std::string& status) {
    const std::string normalized = StringUtils::toLower(status);
    return normalized == "suspended" || normalized == "suspending";
  }

  constexpr int kNvmlSuccess = 0;
  constexpr unsigned int kNvmlTemperatureGpu = 0;

  using NvmlReturn = int;
  using NvmlDevice = struct nvmlDevice_st*;
  using NvmlInitFn = NvmlReturn (*)();
  using NvmlShutdownFn = NvmlReturn (*)();
  using NvmlDeviceGetCountFn = NvmlReturn (*)(unsigned int*);
  using NvmlDeviceGetHandleByIndexFn = NvmlReturn (*)(unsigned int, NvmlDevice*);
  using NvmlDeviceGetTemperatureFn = NvmlReturn (*)(NvmlDevice, unsigned int, unsigned int*);
  struct NvmlUsage {
    unsigned int gpu = 0;
    unsigned int memory = 0;
  };
  using NvmlDeviceGetUsageRatesFn = NvmlReturn (*)(NvmlDevice, NvmlUsage*);

  struct NvmlMemory {
    unsigned long long total = 0;
    unsigned long long free = 0;
    unsigned long long used = 0;
  };
  using NvmlDeviceGetMemoryInfoFn = NvmlReturn (*)(NvmlDevice, NvmlMemory*);

  template <typename T> bool loadDlsymFunction(void* library, const char* name, T& out) {
    void* symbol = dlsym(library, name);
    if (symbol == nullptr) {
      return false;
    }
    std::memcpy(&out, &symbol, sizeof(out));
    return true;
  }

  template <typename T> bool loadDlsymFunction(void* library, const char* preferred, const char* fallback, T& out) {
    return loadDlsymFunction(library, preferred, out) || loadDlsymFunction(library, fallback, out);
  }

  constexpr int kRsmiSuccess = 0;
  constexpr std::uint32_t kRsmiTempTypeEdge = 0;
  constexpr int kRsmiTempCurrent = 0;
  constexpr int kRsmiTempMax = 1;
  constexpr std::size_t kRsmiDeviceNameBufferSize = 128;
  constexpr std::uint32_t kRsmiMaxNumFrequenciesV5 = 32;
  constexpr std::uint32_t kRsmiMaxNumFrequenciesV6 = 33;

  using RsmiReturn = int;
  struct RsmiVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    const char* build = nullptr;
  };
  struct RsmiFrequenciesV5 {
    std::uint32_t numSupported = 0;
    std::uint32_t current = 0;
    std::uint64_t frequency[kRsmiMaxNumFrequenciesV5]{};
  };
  struct RsmiFrequenciesV6 {
    bool hasDeepSleep = false;
    std::uint32_t numSupported = 0;
    std::uint32_t current = 0;
    std::uint64_t frequency[kRsmiMaxNumFrequenciesV6]{};
  };
  using RsmiInitFn = RsmiReturn (*)(std::uint64_t);
  using RsmiShutdownFn = RsmiReturn (*)();
  using RsmiVersionGetFn = RsmiReturn (*)(RsmiVersion*);
  using RsmiNumMonitorDevicesFn = RsmiReturn (*)(std::uint32_t*);
  using RsmiDevNameGetFn = RsmiReturn (*)(std::uint32_t, char*, std::size_t);
  using RsmiDevPowerCapGetFn = RsmiReturn (*)(std::uint32_t, std::uint32_t, std::uint64_t*);
  using RsmiDevTempMetricGetFn = RsmiReturn (*)(std::uint32_t, std::uint32_t, int, std::int64_t*);
  using RsmiDevBusyPercentGetFn = RsmiReturn (*)(std::uint32_t, std::uint32_t*);
  using RsmiDevMemoryBusyPercentGetFn = RsmiReturn (*)(std::uint32_t, std::uint32_t*);
  using RsmiDevGpuClockFreqGetV5Fn = RsmiReturn (*)(std::uint32_t, int, RsmiFrequenciesV5*);
  using RsmiDevGpuClockFreqGetV6Fn = RsmiReturn (*)(std::uint32_t, int, RsmiFrequenciesV6*);
  using RsmiDevPowerAverageGetFn = RsmiReturn (*)(std::uint32_t, std::uint32_t, std::uint64_t*);
  using RsmiDevMemoryTotalGetFn = RsmiReturn (*)(std::uint32_t, int, std::uint64_t*);
  using RsmiDevMemoryUsageGetFn = RsmiReturn (*)(std::uint32_t, int, std::uint64_t*);
  using RsmiDevPciThroughputGetFn = RsmiReturn (*)(std::uint32_t, std::uint64_t*, std::uint64_t*, std::uint64_t*);

  constexpr Logger kLog("sysmon");

  std::uint64_t readZfsEvictableArcKb() {
    std::ifstream file{"/proc/spl/kstat/zfs/arcstats"};
    if (!file.is_open()) {
      return 0;
    }

    std::uint64_t arcSize = 0;
    std::uint64_t arcMin = 0;
    std::string line;
    while (std::getline(file, line)) {
      std::string key;
      std::uint32_t type = 0;
      std::uint64_t value = 0;

      std::istringstream iss{line};
      if (iss >> key >> type >> value) {
        if (key == "size") {
          arcSize = value;
        } else if (key == "c_min") {
          arcMin = value;
        }
      }
    }

    if (arcSize > arcMin) {
      return (arcSize - arcMin) / 1024;
    }
    return 0;
  }

} // namespace

struct SystemMonitorService::AmdRsmiReader {
  ~AmdRsmiReader() { close(); }

  [[nodiscard]] bool ready() { return ensureReady(); }

  [[nodiscard]] std::optional<TempSensorReading> readTempSensor() {
    if (!ensureReady()) {
      return std::nullopt;
    }

    for (std::uint32_t i = 0; i < m_deviceCount; ++i) {
      std::int64_t temp = 0;
      if (m_tempMetricGet(i, kRsmiTempTypeEdge, kRsmiTempCurrent, &temp) != kRsmiSuccess || temp <= 0) {
        continue;
      }

      const double tempC = static_cast<double>(temp) / 1000.0;
      return TempSensorReading{
          .tempC = tempC,
          .score = 0,
          .source = m_deviceCount == 1 ? std::string{"rocm-smi"}
                                       : std::format("rocm-smi:device{}", static_cast<unsigned int>(i))
      };
    }

    return std::nullopt;
  }

  [[nodiscard]] std::optional<SysfsGpuUsageReading> readUsage() {
    if (!ensureReady()) {
      return std::nullopt;
    }

    for (std::uint32_t i = 0; i < m_deviceCount; ++i) {
      std::uint32_t usage = 0;
      if (m_busyPercentGet(i, &usage) != kRsmiSuccess) {
        continue;
      }

      return SysfsGpuUsageReading{
          .percent = static_cast<double>(usage),
          .source = m_deviceCount == 1 ? std::string{"rocm-smi"}
                                       : std::format("rocm-smi:device{}", static_cast<unsigned int>(i)),
      };
    }

    return std::nullopt;
  }

private:
  enum class State { Uninitialized, Unavailable, Ready };

  [[nodiscard]] bool ensureReady() {
    if (m_state == State::Ready) {
      return true;
    }
    if (m_state == State::Unavailable) {
      return false;
    }

    constexpr const char* kLibraries[] = {
        "/opt/rocm/lib/librocm_smi64.so", "librocm_smi64.so",   "librocm_smi64.so.5",
        "librocm_smi64.so.1.0",           "librocm_smi64.so.6", "librocm_smi64.so.7",
    };

    for (const char* library : kLibraries) {
      m_library = dlopen(library, RTLD_LAZY);
      if (m_library != nullptr) {
        break;
      }
    }
    if (m_library == nullptr) {
      m_state = State::Unavailable;
      return false;
    }

    if (!loadDlsymFunction(m_library, "rsmi_init", m_init)
        || !loadDlsymFunction(m_library, "rsmi_shut_down", m_shutdown)
        || !loadDlsymFunction(m_library, "rsmi_version_get", m_versionGet)
        || !loadDlsymFunction(m_library, "rsmi_num_monitor_devices", m_numMonitorDevices)
        || !loadDlsymFunction(m_library, "rsmi_dev_name_get", m_nameGet)
        || !loadDlsymFunction(m_library, "rsmi_dev_power_cap_get", m_powerCapGet)
        || !loadDlsymFunction(m_library, "rsmi_dev_temp_metric_get", m_tempMetricGet)
        || !loadDlsymFunction(m_library, "rsmi_dev_busy_percent_get", m_busyPercentGet)
        || !loadDlsymFunction(m_library, "rsmi_dev_memory_busy_percent_get", m_memoryBusyPercentGet)
        || !loadDlsymFunction(m_library, "rsmi_dev_power_ave_get", m_powerAverageGet)
        || !loadDlsymFunction(m_library, "rsmi_dev_memory_total_get", m_memoryTotalGet)
        || !loadDlsymFunction(m_library, "rsmi_dev_memory_usage_get", m_memoryUsageGet)
        || !loadDlsymFunction(m_library, "rsmi_dev_pci_throughput_get", m_pciThroughputGet)) {
      return failInit();
    }

    if (m_init(0) != kRsmiSuccess) {
      return failInit();
    }
    m_rsmiInitialized = true;

    RsmiVersion version;
    if (m_versionGet(&version) != kRsmiSuccess) {
      return failInit();
    }

    std::uint32_t effectiveMajor = version.major;
    if (version.major == 1) {
      const bool hasV6Symbol = dlsym(m_library, "rsmi_dev_activity_metric_get") != nullptr;
      (void)dlerror();
      effectiveMajor = hasV6Symbol ? 6U : 5U;
    }

    if (effectiveMajor == 5) {
      if (!loadDlsymFunction(m_library, "rsmi_dev_gpu_clk_freq_get", m_gpuClockFreqGetV5)) {
        return failInit();
      }
    } else if (effectiveMajor == 6 || effectiveMajor == 7) {
      if (!loadDlsymFunction(m_library, "rsmi_dev_gpu_clk_freq_get", m_gpuClockFreqGetV6)) {
        return failInit();
      }
    } else {
      return failInit();
    }

    if (m_numMonitorDevices(&m_deviceCount) != kRsmiSuccess || m_deviceCount == 0) {
      return failInit();
    }

    for (std::uint32_t i = 0; i < m_deviceCount; ++i) {
      char name[kRsmiDeviceNameBufferSize]{};
      (void)m_nameGet(i, name, kRsmiDeviceNameBufferSize);

      std::uint64_t maxPower = 0;
      (void)m_powerCapGet(i, 0, &maxPower);

      std::int64_t tempMax = 0;
      (void)m_tempMetricGet(i, kRsmiTempTypeEdge, kRsmiTempMax, &tempMax);
    }

    m_state = State::Ready;
    return true;
  }

  [[nodiscard]] bool failInit() {
    close();
    m_state = State::Unavailable;
    return false;
  }

  void close() {
    if (m_rsmiInitialized && m_shutdown != nullptr) {
      (void)m_shutdown();
    }
    m_rsmiInitialized = false;
    m_deviceCount = 0;

    if (m_library != nullptr) {
      (void)dlclose(m_library);
      m_library = nullptr;
    }
  }

  State m_state = State::Uninitialized;
  void* m_library = nullptr;
  bool m_rsmiInitialized = false;
  std::uint32_t m_deviceCount = 0;
  RsmiInitFn m_init = nullptr;
  RsmiShutdownFn m_shutdown = nullptr;
  RsmiVersionGetFn m_versionGet = nullptr;
  RsmiNumMonitorDevicesFn m_numMonitorDevices = nullptr;
  RsmiDevNameGetFn m_nameGet = nullptr;
  RsmiDevPowerCapGetFn m_powerCapGet = nullptr;
  RsmiDevTempMetricGetFn m_tempMetricGet = nullptr;
  RsmiDevBusyPercentGetFn m_busyPercentGet = nullptr;
  RsmiDevMemoryBusyPercentGetFn m_memoryBusyPercentGet = nullptr;
  RsmiDevGpuClockFreqGetV5Fn m_gpuClockFreqGetV5 = nullptr;
  RsmiDevGpuClockFreqGetV6Fn m_gpuClockFreqGetV6 = nullptr;
  RsmiDevPowerAverageGetFn m_powerAverageGet = nullptr;
  RsmiDevMemoryTotalGetFn m_memoryTotalGet = nullptr;
  RsmiDevMemoryUsageGetFn m_memoryUsageGet = nullptr;
  RsmiDevPciThroughputGetFn m_pciThroughputGet = nullptr;
};

struct SystemMonitorService::NvidiaNvmlReader {
  ~NvidiaNvmlReader() { close(); }

  [[nodiscard]] std::optional<TempSensorReading> readGpuTempSensor() {
    if (!ensureReady() || m_devices.empty()) {
      return std::nullopt;
    }

    std::optional<double> bestTemp;
    unsigned int bestIndex = 0;
    for (const auto& device : m_devices) {
      unsigned int tempC = 0;
      if (m_getTemperature(device.handle, kNvmlTemperatureGpu, &tempC) != kNvmlSuccess || tempC == 0) {
        continue;
      }
      if (!bestTemp.has_value() || static_cast<double>(tempC) > *bestTemp) {
        bestTemp = static_cast<double>(tempC);
        bestIndex = device.index;
      }
    }

    if (!bestTemp.has_value()) {
      return std::nullopt;
    }

    const std::string source = m_devices.size() == 1 ? std::string{"nvml"} : std::format("nvml:device{}", bestIndex);
    return TempSensorReading{.tempC = *bestTemp, .score = 0, .source = source, .isNvidia = true};
  }

  [[nodiscard]] std::optional<double> readGpuUsagePercent() {
    if (!ensureReady() || m_devices.empty()) {
      return std::nullopt;
    }

    double totalUsage = 0.0;
    std::size_t sampledDevices = 0;
    for (const auto& device : m_devices) {
      NvmlUsage usage{};
      if (m_getUsageRates == nullptr || m_getUsageRates(device.handle, &usage) != kNvmlSuccess || usage.gpu > 100U) {
        continue;
      }
      totalUsage += static_cast<double>(usage.gpu);
      ++sampledDevices;
    }

    if (sampledDevices == 0) {
      return std::nullopt;
    }

    return totalUsage / static_cast<double>(sampledDevices);
  }

  [[nodiscard]] std::optional<GpuVramReading> readGpuVram() {
    if (!ensureReady() || m_devices.empty()) {
      return std::nullopt;
    }

    GpuVramReading total{
        .source = m_devices.size() == 1 ? std::string{"nvml"} : std::format("nvml ({} devices)", m_devices.size()),
        .isNvidia = true
    };
    for (const auto& device : m_devices) {
      NvmlMemory memory{};
      if (m_getMemoryInfo(device.handle, &memory) != kNvmlSuccess || memory.total == 0 || memory.used > memory.total) {
        continue;
      }
      total.usedBytes += static_cast<std::uint64_t>(memory.used);
      total.totalBytes += static_cast<std::uint64_t>(memory.total);
    }

    return hasUsableVram(total) ? std::optional<GpuVramReading>{total} : std::nullopt;
  }

private:
  struct DeviceRef {
    unsigned int index = 0;
    NvmlDevice handle = nullptr;
  };

  enum class State { Uninitialized, Unavailable, Ready };

  [[nodiscard]] bool ensureReady() {
    if (m_state == State::Ready) {
      return true;
    }
    if (m_state == State::Unavailable) {
      return false;
    }

    m_library = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (m_library == nullptr) {
      m_state = State::Unavailable;
      return false;
    }

    if (!loadDlsymFunction(m_library, "nvmlInit_v2", "nvmlInit", m_init)
        || !loadDlsymFunction(m_library, "nvmlShutdown", m_shutdown)
        || !loadDlsymFunction(m_library, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount", m_getCount)
        || !loadDlsymFunction(m_library, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex", m_getHandle)
        || !loadDlsymFunction(m_library, "nvmlDeviceGetTemperature", m_getTemperature)
        || !loadDlsymFunction(m_library, "nvmlDeviceGetUtilizationRates", m_getUsageRates)
        || !loadDlsymFunction(m_library, "nvmlDeviceGetMemoryInfo", m_getMemoryInfo)) {
      close();
      m_state = State::Unavailable;
      return false;
    }

    if (m_init() != kNvmlSuccess) {
      close();
      m_state = State::Unavailable;
      return false;
    }
    m_nvmlInitialized = true;

    unsigned int count = 0;
    if (m_getCount(&count) != kNvmlSuccess) {
      close();
      m_state = State::Unavailable;
      return false;
    }

    m_devices.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
      NvmlDevice device = nullptr;
      if (m_getHandle(i, &device) == kNvmlSuccess && device != nullptr) {
        m_devices.push_back({.index = i, .handle = device});
      }
    }

    m_state = State::Ready;
    return true;
  }

  void close() {
    m_devices.clear();

    if (m_nvmlInitialized && m_shutdown != nullptr) {
      (void)m_shutdown();
    }
    m_nvmlInitialized = false;

    if (m_library != nullptr) {
      (void)dlclose(m_library);
      m_library = nullptr;
    }
  }

  State m_state = State::Uninitialized;
  void* m_library = nullptr;
  bool m_nvmlInitialized = false;
  std::vector<DeviceRef> m_devices;
  NvmlInitFn m_init = nullptr;
  NvmlShutdownFn m_shutdown = nullptr;
  NvmlDeviceGetCountFn m_getCount = nullptr;
  NvmlDeviceGetHandleByIndexFn m_getHandle = nullptr;
  NvmlDeviceGetTemperatureFn m_getTemperature = nullptr;
  NvmlDeviceGetUsageRatesFn m_getUsageRates = nullptr;
  NvmlDeviceGetMemoryInfoFn m_getMemoryInfo = nullptr;
};

// Intel exposes no device-wide busy counter, so usage is summed from each DRM client's fdinfo and
// needs the previous scan kept between polls.
struct SystemMonitorService::IntelGpuReader {
  IntelGpuReader() {
    m_devices = noctalia::system::intel_gpu::findDevices();
    // A discrete card is the one that reports VRAM; order it ahead of an integrated GPU so the
    // stats describe the card the user cares about.
    std::ranges::stable_partition(m_devices, [](const noctalia::system::intel_gpu::Device& device) {
      return noctalia::system::intel_gpu::readVram(device).has_value();
    });
  }

  [[nodiscard]] bool ready() const { return !m_devices.empty(); }

  // The first scan only baselines the counters, so name the source before it can report a value.
  [[nodiscard]] std::string usageSource() const {
    return m_devices.empty() ? std::string{} : noctalia::system::intel_gpu::usageSource(m_devices.front());
  }

  [[nodiscard]] std::optional<noctalia::system::intel_gpu::UsageReading> readUsage() {
    for (const auto& device : m_devices) {
      if (const auto reading = m_samplers[device.pciSlot].sample(device); reading.has_value()) {
        return reading;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<noctalia::system::intel_gpu::VramReading> readVram() const {
    for (const auto& device : m_devices) {
      if (const auto reading = noctalia::system::intel_gpu::readVram(device); reading.has_value()) {
        return reading;
      }
    }
    return std::nullopt;
  }

private:
  std::vector<noctalia::system::intel_gpu::Device> m_devices;
  std::unordered_map<std::string, noctalia::system::intel_gpu::UsageSampler> m_samplers;
};

SystemMonitorService::SystemMonitorService(const SystemConfig::MonitorConfig& config) {
  m_latest = makeInitialHistoryStats();
  m_history.fill(m_latest);
  applyConfig(config);
}

SystemMonitorService::~SystemMonitorService() { stop(); }

bool SystemMonitorService::isRunning() const noexcept { return m_running.load(); }

SystemConfig::MonitorConfig SystemMonitorService::pollConfig() const {
  std::scoped_lock lock{m_configMutex};
  return m_pollConfig;
}

std::chrono::steady_clock::duration SystemMonitorService::historySampleInterval() const noexcept {
  std::scoped_lock lock{m_configMutex};
  return m_historyInterval;
}

void SystemMonitorService::applyConfig(const SystemConfig::MonitorConfig& config) {
  const SystemConfig::MonitorConfig sanitized = sanitizeMonitorConfig(config);
  {
    std::scoped_lock lock{m_configMutex};
    m_pollConfig = sanitized;
    m_historyInterval = pollDuration(effectiveHistoryPollSeconds(sanitized));
  }
  {
    // The generation must be published under the wake mutex, or the increment can land after the
    // sampling thread evaluated its predicate but before it blocks, losing the wakeup.
    std::scoped_lock wakeLock{m_wakeMutex};
    m_configGeneration.fetch_add(1, std::memory_order_relaxed);
  }
  m_wakeCv.notify_all();
  setEnabled(sanitized.enabled);
}

void SystemMonitorService::setEnabled(bool enabled) {
  if (enabled) {
    if (!m_running.load()) {
      start();
    }
  } else {
    stop();
  }
}

SystemStats SystemMonitorService::latest() const {
  std::scoped_lock lock{m_statsMutex};
  return m_latest;
}

std::vector<SystemStats> SystemMonitorService::history(int windowSize) const {
  std::scoped_lock lock{m_statsMutex};
  return historyWindowFromRing(m_history, m_historyHead, windowSize);
}

double SystemMonitorService::netRxBytesPerSec(std::string_view interfaceName) const {
  std::scoped_lock lock{m_statsMutex};
  return netRxFromStats(m_latest, interfaceName);
}

double SystemMonitorService::netTxBytesPerSec(std::string_view interfaceName) const {
  std::scoped_lock lock{m_statsMutex};
  return netTxFromStats(m_latest, interfaceName);
}

void SystemMonitorService::retainCpuTemp() { m_cpuTempRefs.fetch_add(1, std::memory_order_relaxed); }

void SystemMonitorService::releaseCpuTemp() { m_cpuTempRefs.fetch_sub(1, std::memory_order_relaxed); }

void SystemMonitorService::retainGpuTemp() { m_gpuTempRefs.fetch_add(1, std::memory_order_relaxed); }

void SystemMonitorService::releaseGpuTemp() { m_gpuTempRefs.fetch_sub(1, std::memory_order_relaxed); }

void SystemMonitorService::retainGpuUsage() { m_gpuUsageRefs.fetch_add(1, std::memory_order_relaxed); }

void SystemMonitorService::releaseGpuUsage() { m_gpuUsageRefs.fetch_sub(1, std::memory_order_relaxed); }

void SystemMonitorService::retainGpuVram() { m_gpuVramRefs.fetch_add(1, std::memory_order_relaxed); }

void SystemMonitorService::releaseGpuVram() { m_gpuVramRefs.fetch_sub(1, std::memory_order_relaxed); }

namespace {
  struct DiskStatvfsData {
    float percent = 0.0f;
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
    std::uint64_t availBytes = 0;
    bool valid = false;
  };

  [[nodiscard]] DiskStatvfsData readDiskStatvfs(const std::string& path) {
    struct statvfs sv{};
    if (::statvfs(path.c_str(), &sv) != 0 || sv.f_blocks == 0) {
      return {};
    }
    const auto total = static_cast<double>(sv.f_blocks);
    const auto freeBlks = static_cast<double>(sv.f_bfree);
    const double used = total - freeBlks;
    const auto frSize = static_cast<std::uint64_t>(sv.f_frsize);
    return DiskStatvfsData{
        .percent = static_cast<float>(100.0 * used / total),
        .totalBytes = static_cast<std::uint64_t>(sv.f_blocks) * frSize,
        .freeBytes = static_cast<std::uint64_t>(sv.f_bfree) * frSize,
        .availBytes = static_cast<std::uint64_t>(sv.f_bavail) * frSize,
        .valid = true,
    };
  }
} // namespace

void SystemMonitorService::retainDiskPath(const std::string& path) {
  const auto initial = readDiskStatvfs(path);
  std::scoped_lock lock{m_statsMutex};
  auto& disk = m_diskHistories[path];
  if (disk.refs == 0) {
    disk.latestPercent = initial.percent;
    disk.latestTotalBytes = initial.totalBytes;
    disk.latestFreeBytes = initial.freeBytes;
    disk.latestAvailBytes = initial.availBytes;
    disk.history.fill(initial.percent);
  }
  ++disk.refs;
}

void SystemMonitorService::releaseDiskPath(const std::string& path) {
  std::scoped_lock lock{m_statsMutex};
  const auto it = m_diskHistories.find(path);
  if (it == m_diskHistories.end()) {
    return;
  }
  --it->second.refs;
  if (it->second.refs <= 0) {
    m_diskHistories.erase(it);
  }
}

float SystemMonitorService::diskUsagePercent(const std::string& path) const {
  std::scoped_lock lock{m_statsMutex};
  const auto it = m_diskHistories.find(path);
  return it != m_diskHistories.end() ? it->second.latestPercent : 0.0f;
}

std::uint64_t SystemMonitorService::diskTotalBytes(const std::string& path) const {
  std::scoped_lock lock{m_statsMutex};
  const auto it = m_diskHistories.find(path);
  return it != m_diskHistories.end() ? it->second.latestTotalBytes : 0;
}

std::uint64_t SystemMonitorService::diskFreeBytes(const std::string& path) const {
  std::scoped_lock lock{m_statsMutex};
  const auto it = m_diskHistories.find(path);
  return it != m_diskHistories.end() ? it->second.latestFreeBytes : 0;
}

std::uint64_t SystemMonitorService::diskAvailBytes(const std::string& path) const {
  std::scoped_lock lock{m_statsMutex};
  const auto it = m_diskHistories.find(path);
  return it != m_diskHistories.end() ? it->second.latestAvailBytes : 0;
}

std::vector<float> SystemMonitorService::diskHistory(const std::string& path, int windowSize) const {
  std::scoped_lock lock{m_statsMutex};
  const auto it = m_diskHistories.find(path);
  if (it == m_diskHistories.end() || windowSize <= 0) {
    return {};
  }
  return historyWindowFromRing(it->second.history, m_historyHead, windowSize);
}

void SystemMonitorService::start() {
  if (m_running.load()) {
    return;
  }

  logDetectedSources();
  m_running = true;
  m_prevNetBytes.clear();
  try {
    m_thread = std::thread([this]() { samplingLoop(); });
  } catch (...) {
    m_running = false;
    throw;
  }
}

void SystemMonitorService::stop() {
  {
    std::scoped_lock wakeLock{m_wakeMutex};
    m_running = false;
  }
  m_wakeCv.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
  releaseGpuReaders();
}

void SystemMonitorService::logDetectedSources() {
  const SystemConfig::MonitorConfig pollCfg = pollConfig();
  const auto cpu = readCpuTotals();
  const auto mem = readMemoryKb();
  const auto net = readNetBytes();
  const auto load = readLoadAvg();

  kLog.info(
      "detected stats sources: cpu={} memory={} network={} load={} disk=statvfs",
      cpu.has_value() ? "/proc/stat" : "unavailable", mem.has_value() ? "/proc/meminfo" : "unavailable",
      net.has_value() ? std::format("/proc/net/dev ({} active)", net->size()) : std::string{"unavailable"},
      load.has_value() ? "/proc/loadavg" : "unavailable"
  );

  const auto cpuTemp = readCpuTempSensor(pollCfg);
  if (cpuTemp.reading.has_value()) {
    kLog.info("detected CPU temperature source: {} ({:.0f}C)", cpuTemp.reading->source, cpuTemp.reading->tempC);
  } else if (!cpuTemp.error.empty()) {
    kLog.warn("detected CPU temperature source: unavailable; {}", cpuTemp.error);
  } else {
    kLog.info("detected CPU temperature source: unavailable");
  }

  // GPU sources are reported from the sampling thread on the first probe: detecting them here would
  // wake a discrete GPU at startup even when nothing displays a GPU stat.
  if (pollCfg.gpuPollSeconds <= 0.0f) {
    kLog.info("GPU monitoring disabled");
  }
}

void SystemMonitorService::samplingLoop() {
  using Clock = std::chrono::steady_clock;

  auto prevCpu = readCpuTotals();
  auto nextCpu = Clock::now();
  auto nextGpu = Clock::now();
  auto nextMemory = Clock::now();
  auto nextNetwork = Clock::now();
  auto nextDisk = Clock::now();
  auto nextHistory = Clock::now();

  while (m_running.load()) {
    const std::uint64_t configGeneration = m_configGeneration.load();
    const SystemConfig::MonitorConfig pollCfg = pollConfig();
    const float historyPollSeconds = effectiveHistoryPollSeconds(pollCfg);

    // A poll value of 0 disables that metric: it is never sampled and never schedules a wakeup.
    const bool cpuEnabled = pollCfg.cpuPollSeconds > 0.0f;
    const bool gpuEnabled = pollCfg.gpuPollSeconds > 0.0f;
    const bool memoryEnabled = pollCfg.memoryPollSeconds > 0.0f;
    const bool networkEnabled = pollCfg.networkPollSeconds > 0.0f;
    const bool diskEnabled = pollCfg.diskPollSeconds > 0.0f;
    const bool historyEnabled = historyPollSeconds > 0.0f;

    if (!gpuEnabled) {
      releaseGpuReaders();
      m_gpuSourcesLogged = false;
    }

    const auto cpuInterval = pollDuration(pollCfg.cpuPollSeconds);
    const auto gpuInterval = pollDuration(pollCfg.gpuPollSeconds);
    const auto memoryInterval = pollDuration(pollCfg.memoryPollSeconds);
    const auto networkInterval = pollDuration(pollCfg.networkPollSeconds);
    const auto diskInterval = pollDuration(pollCfg.diskPollSeconds);
    const auto historyInterval = pollDuration(historyPollSeconds);

    const auto now = Clock::now();
    bool statsTouched = false;

    if (cpuEnabled && now >= nextCpu) {
      const auto currentCpu = readCpuTotals();
      if (prevCpu.has_value() && currentCpu.has_value()) {
        const std::uint64_t totalDelta = currentCpu->total - prevCpu->total;
        const std::uint64_t idleDelta = currentCpu->idle - prevCpu->idle;
        if (totalDelta > 0) {
          std::scoped_lock lock{m_statsMutex};
          m_latest.cpuUsagePercent = 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
        }
      }
      if (currentCpu.has_value()) {
        prevCpu = currentCpu;
      }

      if (const auto la = readLoadAvg(); la.has_value()) {
        std::scoped_lock lock{m_statsMutex};
        m_latest.loadAvg1 = (*la)[0];
        m_latest.loadAvg5 = (*la)[1];
        m_latest.loadAvg15 = (*la)[2];
      }

      if (m_cpuTempRefs.load(std::memory_order_relaxed) > 0) {
        std::optional<double> cpuTemp = readCpuTempCelsius(pollCfg);
        std::scoped_lock lock{m_statsMutex};
        if (cpuTemp.has_value()) {
          m_latest.cpuTempC = cpuTemp;
          m_latest.cpuTempAvailable = true;
        } else if (!m_latest.cpuTempC.has_value()) {
          m_latest.cpuTempC = 40.0;
          m_latest.cpuTempAvailable = false;
        }
      }

      nextCpu = now + cpuInterval;
      statsTouched = true;
    }

    if (memoryEnabled && now >= nextMemory) {
      if (const auto memKb = readMemoryKb(); memKb.has_value()) {
        std::scoped_lock lock{m_statsMutex};
        m_latest.ramTotalMb = memKb->totalKb / 1024;
        m_latest.ramUsedMb = memKb->usedKb / 1024;
        if (memKb->totalKb > 0) {
          m_latest.ramUsagePercent = 100.0 * static_cast<double>(memKb->usedKb) / static_cast<double>(memKb->totalKb);
        }
      }
      nextMemory = now + memoryInterval;
      statsTouched = true;
    }

    if (networkEnabled && now >= nextNetwork) {
      if (const auto currentNetBytes = readNetBytes(); currentNetBytes.has_value()) {
        const double intervalSeconds = std::chrono::duration<double>(networkInterval).count();
        const double scale = intervalSeconds > 0.0 ? 1.0 / intervalSeconds : 1.0;
        double totalRx = 0.0;
        double totalTx = 0.0;
        std::unordered_map<std::string, SystemStats::NetThroughput> byInterface;
        for (const auto& [iface, cur] : *currentNetBytes) {
          const auto it = m_prevNetBytes.find(iface);
          double ifaceRx = 0.0;
          double ifaceTx = 0.0;
          if (it != m_prevNetBytes.end()) {
            if (cur.rx >= it->second.rx) {
              ifaceRx = static_cast<double>(cur.rx - it->second.rx) * scale;
            }
            if (cur.tx >= it->second.tx) {
              ifaceTx = static_cast<double>(cur.tx - it->second.tx) * scale;
            }
          }
          if (iface != "lo") {
            totalRx += ifaceRx;
            totalTx += ifaceTx;
          }
          byInterface.emplace(iface, SystemStats::NetThroughput{.rxBytesPerSec = ifaceRx, .txBytesPerSec = ifaceTx});
        }
        m_prevNetBytes = *currentNetBytes;
        std::scoped_lock lock{m_statsMutex};
        m_latest.netRxBytesPerSec = totalRx;
        m_latest.netTxBytesPerSec = totalTx;
        m_latest.netThroughputByInterface = std::move(byInterface);
      }
      nextNetwork = now + networkInterval;
      statsTouched = true;
    }

    if (gpuEnabled && now >= nextGpu) {
      const bool pollGpuTemp = m_gpuTempRefs.load(std::memory_order_relaxed) > 0;
      const bool pollGpuUsage = m_gpuUsageRefs.load(std::memory_order_relaxed) > 0;
      const bool pollGpuVram = m_gpuVramRefs.load(std::memory_order_relaxed) > 0;

      if (pollGpuTemp || pollGpuUsage || pollGpuVram) {
        const NvidiaDisplayDeviceState nvidiaDisplayState = detectNvidiaPciDisplayDeviceState();
        // The first probe doubles as source detection: it reports what each retained stat resolved to.
        const bool logSources = !m_gpuSourcesLogged;
        m_gpuSourcesLogged = true;

        if (pollGpuTemp) {
          const auto gpuTemp = readGpuTempData(nvidiaDisplayState);
          if (logSources) {
            if (gpuTemp.tempC.has_value()) {
              kLog.info(
                  "detected GPU temperature source: {} ({:.0f}C); {}", gpuTemp.source, *gpuTemp.tempC, gpuTemp.detail
              );
            } else {
              kLog.info("detected GPU temperature source: unavailable; {}", gpuTemp.detail);
            }
          }
          std::scoped_lock lock{m_statsMutex};
          if (gpuTemp.tempC.has_value()) {
            m_latest.gpuTempC = gpuTemp.tempC;
          }
        }
        if (pollGpuUsage) {
          const auto gpuUsage = readGpuUsageData(nvidiaDisplayState);
          if (logSources) {
            if (gpuUsage.percent.has_value()) {
              kLog.info("detected GPU usage source: {} ({:.0f}%)", gpuUsage.source, *gpuUsage.percent);
            } else if (!gpuUsage.source.empty()) {
              // Counter-delta sources have nothing to report until their second sample.
              kLog.info("detected GPU usage source: {} (awaiting first sample)", gpuUsage.source);
            } else {
              kLog.info("detected GPU usage source: unavailable");
            }
          }
          std::scoped_lock lock{m_statsMutex};
          if (gpuUsage.percent.has_value()) {
            m_latest.gpuUsagePercent = gpuUsage.percent;
          }
        }
        if (pollGpuVram) {
          const auto gpuVram = readGpuVramData(nvidiaDisplayState);
          if (logSources) {
            if (gpuVram.has_value()) {
              kLog.info(
                  "detected GPU VRAM source: {} ({} / {})", gpuVram->source,
                  FormatUnits::formatBinaryBytesAsGib(gpuVram->usedBytes),
                  FormatUnits::formatBinaryBytesAsGib(gpuVram->totalBytes)
              );
            } else {
              kLog.info("detected GPU VRAM source: unavailable");
            }
          }
          if (gpuVram.has_value()) {
            std::scoped_lock lock{m_statsMutex};
            m_latest.gpuVramUsedBytes = gpuVram->usedBytes;
            m_latest.gpuVramTotalBytes = gpuVram->totalBytes;
          }
        }
      }
      nextGpu = now + gpuInterval;
      statsTouched = true;
    }

    if (diskEnabled && now >= nextDisk) {
      if (const auto memKb = readMemoryKb(); memKb.has_value()) {
        std::scoped_lock lock{m_statsMutex};
        m_latest.swapTotalMb = memKb->swapTotalKb / 1024;
        m_latest.swapUsedMb = memKb->swapUsedKb / 1024;
      }
      std::vector<std::string> diskPaths;
      {
        std::scoped_lock lock{m_statsMutex};
        diskPaths.reserve(m_diskHistories.size());
        for (const auto& [path, disk] : m_diskHistories) {
          if (disk.refs > 0) {
            diskPaths.push_back(path);
          }
        }
      }
      for (const auto& path : diskPaths) {
        const auto data = readDiskStatvfs(path);
        std::scoped_lock lock{m_statsMutex};
        const auto it = m_diskHistories.find(path);
        if (it != m_diskHistories.end() && it->second.refs > 0) {
          it->second.latestPercent = data.percent;
          it->second.latestTotalBytes = data.totalBytes;
          it->second.latestFreeBytes = data.freeBytes;
          it->second.latestAvailBytes = data.availBytes;
        }
      }
      nextDisk = now + diskInterval;
    }

    if (statsTouched) {
      std::scoped_lock lock{m_statsMutex};
      m_latest.sampledAt = now;
    }

    if (historyEnabled && now >= nextHistory) {
      std::scoped_lock lock{m_statsMutex};
      const auto writeIndex = static_cast<std::size_t>(m_historyHead);
      m_history[writeIndex] = m_latest;
      for (auto& [path, disk] : m_diskHistories) {
        if (disk.refs <= 0) {
          continue;
        }
        (void)path;
        disk.history[writeIndex] = disk.latestPercent;
      }
      m_historyHead = (m_historyHead + 1) % kHistorySize;
      nextHistory = now + historyInterval;
    }

    // Only enabled metrics schedule a wakeup; if all are disabled we sleep until stopped or
    // until a config change re-enables one (applyConfig notifies the wake cv).
    auto nextWake = Clock::time_point::max();
    const auto considerWake = [&](bool enabled, Clock::time_point at) {
      if (enabled) {
        nextWake = std::min(nextWake, at);
      }
    };
    considerWake(cpuEnabled, nextCpu);
    considerWake(gpuEnabled, nextGpu);
    considerWake(memoryEnabled, nextMemory);
    considerWake(networkEnabled, nextNetwork);
    considerWake(diskEnabled, nextDisk);
    considerWake(historyEnabled, nextHistory);

    std::unique_lock wakeLock{m_wakeMutex};
    m_wakeCv.wait_until(wakeLock, nextWake, [this, configGeneration]() {
      return !m_running.load() || m_configGeneration.load() != configGeneration;
    });
  }
}

void SystemMonitorService::releaseGpuReaders() {
  m_nvidiaNvmlReader.reset();
  m_amdRsmiReader.reset();
  m_intelGpuReader.reset();
}

std::optional<SystemMonitorService::CpuTotals> SystemMonitorService::readCpuTotals() {
  std::ifstream file{"/proc/stat"};
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string line;
  if (!std::getline(file, line)) {
    return std::nullopt;
  }

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

  iss >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
  if (cpuLabel != "cpu") {
    return std::nullopt;
  }

  CpuTotals totals{};
  totals.idle = idle + iowait;
  totals.total = user + nice + system + idle + iowait + irq + softirq + steal;
  return totals;
}

std::optional<SystemMonitorService::MemData> SystemMonitorService::readMemoryKb() {
  std::ifstream file{"/proc/meminfo"};
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string key;
  std::uint64_t value_kb = 0;
  std::string unit;

  std::uint64_t totalKb = 0;
  std::uint64_t availableKb = 0;
  std::uint64_t swapTotalKb = 0;
  std::uint64_t swapFreeKb = 0;

  while (file >> key >> value_kb >> unit) {
    if (key == "MemTotal:") {
      totalKb = value_kb;
    } else if (key == "MemAvailable:") {
      availableKb = value_kb;
    } else if (key == "SwapTotal:") {
      swapTotalKb = value_kb;
    } else if (key == "SwapFree:") {
      swapFreeKb = value_kb;
    }

    // SwapFree appears last, after that there's nothing we need
    if (key == "SwapFree:") {
      break;
    }
  }

  if (totalKb == 0 || availableKb == 0 || availableKb > totalKb) {
    return std::nullopt;
  }

  std::uint64_t zfsArcKb = readZfsEvictableArcKb();
  availableKb = std::min(availableKb + zfsArcKb, totalKb);

  MemData data;
  data.totalKb = totalKb;
  data.usedKb = totalKb - availableKb;
  data.swapTotalKb = swapTotalKb;
  data.swapUsedKb = swapTotalKb > swapFreeKb ? swapTotalKb - swapFreeKb : 0;
  return data;
}

std::optional<double> SystemMonitorService::readCpuTempCelsius(const SystemConfig::MonitorConfig& config) {
  const auto reading = readCpuTempSensor(config);
  return reading.reading.has_value() ? std::optional<double>{reading.reading->tempC} : std::nullopt;
}

SystemMonitorService::NvidiaDisplayDeviceState SystemMonitorService::detectNvidiaPciDisplayDeviceState() {
  namespace fs = std::filesystem;

  const fs::path pciRoot{"/sys/bus/pci/devices"};
  if (!fs::exists(pciRoot) || !fs::is_directory(pciRoot)) {
    return NvidiaDisplayDeviceState::None;
  }

  bool foundInactiveNvidiaDisplay = false;
  for (const auto& entry : fs::directory_iterator{pciRoot}) {
    if (!entry.is_directory()) {
      continue;
    }

    const std::string vendor = StringUtils::toLower(FileUtils::readSmallTextFile(entry.path() / "vendor").value_or(""));
    if (vendor != "0x10de") {
      continue;
    }

    const std::string deviceClass =
        StringUtils::toLower(FileUtils::readSmallTextFile(entry.path() / "class").value_or(""));
    if (!deviceClass.starts_with("0x03")) {
      continue;
    }

    const auto runtimeStatus = FileUtils::readSmallTextFile(entry.path() / "power" / "runtime_status");
    if (runtimeStatus.has_value() && isInactiveRuntimeStatus(*runtimeStatus)) {
      foundInactiveNvidiaDisplay = true;
      continue;
    }
    return NvidiaDisplayDeviceState::Active;
  }

  return foundInactiveNvidiaDisplay ? NvidiaDisplayDeviceState::InactiveOnly : NvidiaDisplayDeviceState::None;
}

SystemMonitorService::NvidiaNvmlReader& SystemMonitorService::ensureNvmlReader() {
  if (m_nvidiaNvmlReader == nullptr) {
    m_nvidiaNvmlReader = std::make_unique<NvidiaNvmlReader>();
  }
  return *m_nvidiaNvmlReader;
}

SystemMonitorService::AmdRsmiReader& SystemMonitorService::ensureAmdRsmiReader() {
  if (m_amdRsmiReader == nullptr) {
    m_amdRsmiReader = std::make_unique<AmdRsmiReader>();
  }
  return *m_amdRsmiReader;
}

SystemMonitorService::IntelGpuReader& SystemMonitorService::ensureIntelGpuReader() {
  if (m_intelGpuReader == nullptr) {
    m_intelGpuReader = std::make_unique<IntelGpuReader>();
  }
  return *m_intelGpuReader;
}

SystemMonitorService::GpuTempData SystemMonitorService::readGpuTempData(NvidiaDisplayDeviceState nvidiaDisplayState) {
  switch (nvidiaDisplayState) {
  case NvidiaDisplayDeviceState::Active: {
    const auto nvml = ensureNvmlReader().readGpuTempSensor();
    return GpuTempData{
        .tempC = nvml.has_value() ? std::optional<double>{nvml->tempC} : std::nullopt,
        .source = nvml.has_value() ? nvml->source : std::string{},
        .detail = nvml.has_value() ? "NVML-only mode active" : "NVML-only mode active; NVML unavailable"
    };
  }
  case NvidiaDisplayDeviceState::InactiveOnly: {
    AmdRsmiReader& amdRsmiReader = ensureAmdRsmiReader();
    if (const auto amdRsmi = amdRsmiReader.readTempSensor(); amdRsmi.has_value()) {
      return GpuTempData{
          .tempC = amdRsmi->tempC, .source = amdRsmi->source, .detail = "NVML skipped; using ROCm SMI edge temperature"
      };
    }
    if (amdRsmiReader.ready()) {
      return GpuTempData{
          .tempC = std::nullopt, .source = {}, .detail = "NVML skipped; ROCm SMI temperature unavailable"
      };
    }
    if (const auto amdSysfs = readAmdGpuSysfsTempSensor(); amdSysfs.has_value()) {
      return GpuTempData{
          .tempC = amdSysfs->tempC, .source = amdSysfs->source, .detail = "NVML skipped; using amdgpu sysfs temp1_input"
      };
    }
    const GpuHwmonProbe hwmon = readGpuHwmonTempSensor();
    return GpuTempData{
        .tempC = hwmon.reading.has_value() ? std::optional<double>{hwmon.reading->tempC} : std::nullopt,
        .source = hwmon.reading.has_value() ? hwmon.reading->source : std::string{},
        .detail = "NVML skipped; NVIDIA display device is runtime-suspended"
    };
  }
  case NvidiaDisplayDeviceState::None:
    break;
  }

  AmdRsmiReader& amdRsmiReader = ensureAmdRsmiReader();
  if (const auto amdRsmi = amdRsmiReader.readTempSensor(); amdRsmi.has_value()) {
    return GpuTempData{.tempC = amdRsmi->tempC, .source = amdRsmi->source, .detail = "using ROCm SMI edge temperature"};
  }
  if (amdRsmiReader.ready()) {
    return GpuTempData{.tempC = std::nullopt, .source = {}, .detail = "ROCm SMI temperature unavailable"};
  }

  if (const auto amdSysfs = readAmdGpuSysfsTempSensor(); amdSysfs.has_value()) {
    return GpuTempData{
        .tempC = amdSysfs->tempC, .source = amdSysfs->source, .detail = "using amdgpu sysfs temp1_input"
    };
  }

  const GpuHwmonProbe hwmon = readGpuHwmonTempSensor();
  if (hwmon.foundNvidia) {
    return GpuTempData{
        .tempC = hwmon.reading.has_value() ? std::optional<double>{hwmon.reading->tempC} : std::nullopt,
        .source = hwmon.reading.has_value() ? hwmon.reading->source : std::string{},
        .detail = "NVIDIA hwmon present; NVML fallback not needed"
    };
  }

  std::optional<TempSensorReading> best = hwmon.reading;
  if (!hwmon.foundNvidia) {
    const auto nvml = ensureNvmlReader().readGpuTempSensor();
    if (nvml.has_value() && (!best.has_value() || nvml->tempC > best->tempC)) {
      best = nvml;
    }
    return GpuTempData{
        .tempC = best.has_value() ? std::optional<double>{best->tempC} : std::nullopt,
        .source = best.has_value() ? best->source : std::string{},
        .detail = nvml.has_value() ? "NVML fallback available" : "NVML fallback unavailable"
    };
  }

  return GpuTempData{};
}

SystemMonitorService::GpuUsageData SystemMonitorService::readGpuUsageData(NvidiaDisplayDeviceState nvidiaDisplayState) {
  switch (nvidiaDisplayState) {
  case NvidiaDisplayDeviceState::Active:
    if (const auto gpuUsage = ensureNvmlReader().readGpuUsagePercent(); gpuUsage.has_value()) {
      return GpuUsageData{.percent = gpuUsage, .source = "nvml"};
    }
    return GpuUsageData{};
  case NvidiaDisplayDeviceState::InactiveOnly: {
    AmdRsmiReader& amdRsmiReader = ensureAmdRsmiReader();
    if (const auto rsmi = amdRsmiReader.readUsage(); rsmi.has_value()) {
      return GpuUsageData{.percent = rsmi->percent, .source = rsmi->source};
    }
    if (amdRsmiReader.ready()) {
      return GpuUsageData{};
    }
    if (const auto sysfs = readAmdGpuSysfsUsage(); sysfs.has_value()) {
      return GpuUsageData{.percent = sysfs->percent, .source = sysfs->source};
    }
    // An Optimus laptop with the discrete GPU asleep renders on the Intel integrated GPU.
    return readIntelGpuUsageData();
  }
  case NvidiaDisplayDeviceState::None:
    break;
  }

  {
    AmdRsmiReader& amdRsmiReader = ensureAmdRsmiReader();
    if (const auto rsmi = amdRsmiReader.readUsage(); rsmi.has_value()) {
      return GpuUsageData{.percent = rsmi->percent, .source = rsmi->source};
    }
    if (amdRsmiReader.ready()) {
      return GpuUsageData{};
    }
  }

  if (const auto sysfs = readAmdGpuSysfsUsage(); sysfs.has_value()) {
    return GpuUsageData{.percent = sysfs->percent, .source = sysfs->source};
  }

  if (const auto gpuUsage = ensureNvmlReader().readGpuUsagePercent(); gpuUsage.has_value()) {
    return GpuUsageData{.percent = gpuUsage, .source = "nvml"};
  }

  return readIntelGpuUsageData();
}

SystemMonitorService::GpuUsageData SystemMonitorService::readIntelGpuUsageData() {
  IntelGpuReader& reader = ensureIntelGpuReader();
  if (!reader.ready()) {
    return GpuUsageData{};
  }
  if (const auto usage = reader.readUsage(); usage.has_value()) {
    return GpuUsageData{.percent = usage->percent, .source = usage->source};
  }
  return GpuUsageData{.percent = std::nullopt, .source = reader.usageSource()};
}

std::optional<SystemMonitorService::GpuVramData> SystemMonitorService::readIntelGpuVram() {
  IntelGpuReader& reader = ensureIntelGpuReader();
  if (!reader.ready()) {
    return std::nullopt;
  }
  const auto vram = reader.readVram();
  if (!vram.has_value()) {
    return std::nullopt;
  }
  return GpuVramData{.usedBytes = vram->usedBytes, .totalBytes = vram->totalBytes, .source = vram->source};
}

std::optional<SystemMonitorService::GpuVramData>
SystemMonitorService::readGpuVramData(NvidiaDisplayDeviceState nvidiaDisplayState) {
  switch (nvidiaDisplayState) {
  case NvidiaDisplayDeviceState::Active: {
    const auto nvml = ensureNvmlReader().readGpuVram();
    if (!nvml.has_value() || !hasUsableVram(*nvml)) {
      return std::nullopt;
    }
    return GpuVramData{.usedBytes = nvml->usedBytes, .totalBytes = nvml->totalBytes, .source = nvml->source};
  }
  case NvidiaDisplayDeviceState::InactiveOnly: {
    std::optional<GpuVramReading> combined = readAmdGpuVram();
    if (!combined.has_value()) {
      if (const auto intel = readIntelGpuVram(); intel.has_value()) {
        combined = GpuVramReading{
            .usedBytes = intel->usedBytes, .totalBytes = intel->totalBytes, .source = intel->source, .isNvidia = false
        };
      }
    }
    if (!combined.has_value() || !hasUsableVram(*combined)) {
      return std::nullopt;
    }
    return GpuVramData{
        .usedBytes = combined->usedBytes, .totalBytes = combined->totalBytes, .source = combined->source
    };
  }
  case NvidiaDisplayDeviceState::None:
    break;
  }

  std::optional<GpuVramReading> combined = readAmdGpuVram();

  const auto nvml = ensureNvmlReader().readGpuVram();
  if (nvml.has_value()) {
    if (combined.has_value()) {
      mergeGpuVram(*combined, *nvml);
    } else {
      combined = nvml;
    }
  }

  if (const auto intel = readIntelGpuVram(); intel.has_value()) {
    const GpuVramReading reading{
        .usedBytes = intel->usedBytes, .totalBytes = intel->totalBytes, .source = intel->source, .isNvidia = false
    };
    if (combined.has_value()) {
      mergeGpuVram(*combined, reading);
    } else {
      combined = reading;
    }
  }

  if (!combined.has_value() || !hasUsableVram(*combined)) {
    return std::nullopt;
  }
  return GpuVramData{.usedBytes = combined->usedBytes, .totalBytes = combined->totalBytes, .source = combined->source};
}

std::optional<double> SystemMonitorService::readGpuTempCelsius() {
  return readGpuTempData(detectNvidiaPciDisplayDeviceState()).tempC;
}

std::optional<double> SystemMonitorService::readGpuUsagePercent() {
  return readGpuUsageData(detectNvidiaPciDisplayDeviceState()).percent;
}

std::optional<SystemMonitorService::GpuVramData> SystemMonitorService::readGpuVram() {
  return readGpuVramData(detectNvidiaPciDisplayDeviceState());
}

std::optional<std::unordered_map<std::string, SystemMonitorService::NetIfaceBytes>>
SystemMonitorService::readNetBytes() {
  std::ifstream file{"/proc/net/dev"};
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::unordered_map<std::string, NetIfaceBytes> result;
  std::string line;
  // Skip 2 header lines
  std::getline(file, line);
  std::getline(file, line);

  while (std::getline(file, line)) {
    const auto colonPos = line.find(':');
    if (colonPos == std::string::npos) {
      continue;
    }

    std::string iface = line.substr(0, colonPos);
    while (!iface.empty() && iface.front() == ' ') {
      iface.erase(iface.begin());
    }

    std::istringstream iss{line.substr(colonPos + 1)};
    std::uint64_t rxBytes = 0;
    std::uint64_t val = 0;
    iss >> rxBytes;
    // Skip 7 fields (rx_packets, errs, drop, fifo, frame, compressed, multicast)
    for (int i = 0; i < 7 && iss; ++i) {
      iss >> val;
    }
    std::uint64_t txBytes = 0;
    iss >> txBytes;

    if (rxBytes == 0 && txBytes == 0) {
      continue;
    }

    result[iface] = {rxBytes, txBytes};
  }

  return result;
}

std::optional<std::array<double, 3>> SystemMonitorService::readLoadAvg() {
  std::ifstream file{"/proc/loadavg"};
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::array<double, 3> la{};
  file >> la[0] >> la[1] >> la[2];
  if (file.fail()) {
    return std::nullopt;
  }
  return la;
}
