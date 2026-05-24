#include "system/system_monitor_service.h"

#include "core/log.h"
#include "system/format_units.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
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

  std::optional<std::string> readSmallTextFile(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::string text;
    std::getline(file, text);
    if (text.empty()) {
      return std::nullopt;
    }

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
      text.pop_back();
    }
    return text;
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

  [[nodiscard]] bool hasUsableVram(const GpuVramReading& reading) {
    return reading.totalBytes > 0 && reading.usedBytes <= reading.totalBytes;
  }

  [[nodiscard]] float clampPollSeconds(float seconds) noexcept {
    return std::clamp(
        seconds, SystemConfig::MonitorConfig::kMinPollSeconds, SystemConfig::MonitorConfig::kMaxPollSeconds
    );
  }

  // Graph history snapshots and scroll timing follow the fastest metric poll so users
  // only configure how often each stat is read, not a separate graph-only cadence.
  [[nodiscard]] float effectiveHistoryPollSeconds(const SystemConfig::MonitorConfig& config) noexcept {
    return std::min(
        {config.cpuPollSeconds, config.gpuPollSeconds, config.memoryPollSeconds, config.networkPollSeconds,
         config.diskPollSeconds}
    );
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

  std::string formatThermalZoneTempSource(const std::string& zoneType, const std::filesystem::path& inputPath) {
    const std::string type = zoneType.empty() ? "unknown" : zoneType;
    return std::format("thermal_zone:{} {}", type, inputPath.string());
  }

  int scoreHwmonSensor(const std::string& hwmonName, const std::string& label) {
    int score = 0;
    const std::string name = StringUtils::toLower(hwmonName);
    const std::string lbl = StringUtils::toLower(label);

    if (name.find("coretemp") != std::string::npos || name.find("k10temp") != std::string::npos ||
        name.find("zenpower") != std::string::npos || name.find("cpu") != std::string::npos) {
      score += 20;
    }

    // AMD k10temp exposes Tctl first, but Tctl is a fan-control value. Prefer
    // physical die/CCD readings when the kernel provides them.
    if (lbl.find("tdie") != std::string::npos) {
      score += 80;
    } else if (lbl.find("tccd") != std::string::npos) {
      score += 70;
    } else if (lbl.find("package") != std::string::npos) {
      score += 60;
    } else if (lbl.find("cpu") != std::string::npos) {
      score += 50;
    } else if (lbl.find("tctl") != std::string::npos) {
      score += 40;
    }

    return score;
  }

  bool isBetterHwmonSensor(int score, double tempC, int bestScore, const std::optional<double>& bestTemp) {
    return score > bestScore || (score == bestScore && (!bestTemp.has_value() || tempC > *bestTemp));
  }

  bool isCpuThermalZoneType(const std::string& type) {
    const std::string t = StringUtils::toLower(type);
    return t.find("x86_pkg_temp") != std::string::npos || t.find("cpu") != std::string::npos ||
           t.find("soc") != std::string::npos || t.find("package") != std::string::npos;
  }

  int scoreGpuHwmonSensor(const std::string& hwmonName, const std::string& label) {
    const std::string name = StringUtils::toLower(hwmonName);
    const std::string lbl = StringUtils::toLower(label);

    int score = 0;
    if (name == "amdgpu") {
      score += 20;
    } else if (name == "nvidia" || name.find("nvidia") != std::string::npos) {
      score += 20;
    } else if (name == "i915" || name == "xe") {
      score += 20;
    } else if (name == "nouveau") {
      score += 10;
    } else {
      return -1;
    }

    if (lbl.find("junction") != std::string::npos || lbl.find("edge") != std::string::npos) {
      score += 30;
    } else if (lbl.find("gpu") != std::string::npos || lbl.find("mem") != std::string::npos) {
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
    const auto status = readSmallTextFile(deviceLink / "power" / "runtime_status");
    if (!status.has_value()) {
      return true;
    }
    return *status == "active";
  }

  bool isDevicePathAwake(const std::filesystem::path& devicePath) {
    const auto status = readSmallTextFile(devicePath / "power" / "runtime_status");
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

  std::optional<GpuVramReading> readAmdGpuVram() {
    namespace fs = std::filesystem;

    const fs::path drmRoot{"/sys/class/drm"};
    if (!fs::exists(drmRoot) || !fs::is_directory(drmRoot)) {
      return std::nullopt;
    }

    GpuVramReading total;
    int deviceCount = 0;
    std::string firstSource;

    for (const auto& entry : fs::directory_iterator{drmRoot}) {
      if (!entry.is_directory() || !isDrmCardName(entry.path().filename().string())) {
        continue;
      }

      const fs::path devicePath = entry.path() / "device";
      if (!fs::exists(devicePath) || !isDevicePathAwake(devicePath)) {
        continue;
      }

      const fs::path usedPath = devicePath / "mem_info_vram_used";
      const fs::path totalPath = devicePath / "mem_info_vram_total";
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

  std::optional<TempSensorReading> readCpuHwmonTempSensor() {
    namespace fs = std::filesystem;

    const fs::path hwmonRoot{"/sys/class/hwmon"};
    if (!fs::exists(hwmonRoot) || !fs::is_directory(hwmonRoot)) {
      return std::nullopt;
    }

    int bestScore = -1;
    std::optional<TempSensorReading> best;

    for (const auto& hwmonEntry : fs::directory_iterator{hwmonRoot}) {
      if (!hwmonEntry.is_directory()) {
        continue;
      }

      const std::string hwmonName = readSmallTextFile(hwmonEntry.path() / "name").value_or("");
      for (const auto& fileEntry : fs::directory_iterator{hwmonEntry.path()}) {
        if (!fileEntry.is_regular_file()) {
          continue;
        }

        const std::string fileName = fileEntry.path().filename().string();
        if (!fileName.starts_with("temp") || !fileName.ends_with("_input")) {
          continue;
        }

        const std::string base = fileName.substr(0, fileName.size() - 6);
        const std::string label = readSmallTextFile(hwmonEntry.path() / (base + "_label")).value_or("");
        const auto tempC = readTempInputCelsius(fileEntry.path());
        if (!tempC.has_value()) {
          continue;
        }

        const int score = scoreHwmonSensor(hwmonName, label);
        if (score <= 0) {
          continue;
        }
        if (isBetterHwmonSensor(
                score, *tempC, bestScore, best.has_value() ? std::optional<double>{best->tempC} : std::nullopt
            )) {
          bestScore = score;
          best = TempSensorReading{
              .tempC = *tempC, .score = score, .source = formatHwmonTempSource(hwmonName, label, fileEntry.path())
          };
        }
      }
    }

    return best;
  }

  std::optional<TempSensorReading> readCpuThermalZoneTempSensor() {
    namespace fs = std::filesystem;

    const fs::path thermalRoot{"/sys/class/thermal"};
    if (!fs::exists(thermalRoot) || !fs::is_directory(thermalRoot)) {
      return std::nullopt;
    }

    std::optional<TempSensorReading> fallback;
    for (const auto& entry : fs::directory_iterator{thermalRoot}) {
      if (!entry.is_directory()) {
        continue;
      }

      const auto zoneName = entry.path().filename().string();
      if (!zoneName.starts_with("thermal_zone")) {
        continue;
      }

      const std::string zoneType = readSmallTextFile(entry.path() / "type").value_or("");
      const fs::path tempPath = entry.path() / "temp";
      const auto tempC = readTempInputCelsius(tempPath);
      if (!tempC.has_value()) {
        continue;
      }

      TempSensorReading reading{.tempC = *tempC, .score = 0, .source = formatThermalZoneTempSource(zoneType, tempPath)};
      if (isCpuThermalZoneType(zoneType)) {
        return reading;
      }

      if (!fallback.has_value()) {
        fallback = std::move(reading);
      }
    }

    return fallback;
  }

  std::optional<TempSensorReading> readCpuTempSensor() {
    if (const auto hwmon = readCpuHwmonTempSensor(); hwmon.has_value()) {
      return hwmon;
    }
    return readCpuThermalZoneTempSensor();
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

      const std::string hwmonName = readSmallTextFile(hwmonEntry.path() / "name").value_or("");
      const int nameScore = scoreGpuHwmonSensor(hwmonName, "");
      if (nameScore < 0) {
        continue;
      }

      const std::string normalizedName = StringUtils::toLower(hwmonName);
      const bool isNvidia = normalizedName == "nvidia" || normalizedName.find("nvidia") != std::string::npos;

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
        const std::string label = readSmallTextFile(hwmonEntry.path() / (base + "_label")).value_or("");
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

  bool hasInactiveNvidiaPciDisplayDevice() {
    namespace fs = std::filesystem;

    const fs::path pciRoot{"/sys/bus/pci/devices"};
    if (!fs::exists(pciRoot) || !fs::is_directory(pciRoot)) {
      return false;
    }

    for (const auto& entry : fs::directory_iterator{pciRoot}) {
      if (!entry.is_directory()) {
        continue;
      }

      const std::string vendor = StringUtils::toLower(readSmallTextFile(entry.path() / "vendor").value_or(""));
      if (vendor != "0x10de") {
        continue;
      }

      const std::string deviceClass = StringUtils::toLower(readSmallTextFile(entry.path() / "class").value_or(""));
      if (!deviceClass.starts_with("0x03")) {
        continue;
      }

      const auto runtimeStatus = readSmallTextFile(entry.path() / "power" / "runtime_status");
      if (runtimeStatus.has_value() && isInactiveRuntimeStatus(*runtimeStatus)) {
        return true;
      }
    }

    return false;
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

  constexpr Logger kLog("sysmon");

} // namespace

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

    if (!loadDlsymFunction(m_library, "nvmlInit_v2", "nvmlInit", m_init) ||
        !loadDlsymFunction(m_library, "nvmlShutdown", m_shutdown) ||
        !loadDlsymFunction(m_library, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount", m_getCount) ||
        !loadDlsymFunction(m_library, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex", m_getHandle) ||
        !loadDlsymFunction(m_library, "nvmlDeviceGetTemperature", m_getTemperature) ||
        !loadDlsymFunction(m_library, "nvmlDeviceGetMemoryInfo", m_getMemoryInfo)) {
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
  NvmlDeviceGetMemoryInfoFn m_getMemoryInfo = nullptr;
};

SystemMonitorService::SystemMonitorService(const SystemConfig::MonitorConfig& config) {
  m_latest = makeInitialHistoryStats();
  m_history.fill(m_latest);
  applyConfig(config);
}

SystemMonitorService::~SystemMonitorService() { stop(); }

bool SystemMonitorService::isRunning() const noexcept { return m_running.load(); }

SystemConfig::MonitorConfig SystemMonitorService::pollConfig() const {
  std::lock_guard lock{m_configMutex};
  return m_pollConfig;
}

std::chrono::steady_clock::duration SystemMonitorService::historySampleInterval() const noexcept {
  std::lock_guard lock{m_configMutex};
  return m_historyInterval;
}

void SystemMonitorService::applyConfig(const SystemConfig::MonitorConfig& config) {
  const SystemConfig::MonitorConfig sanitized = sanitizeMonitorConfig(config);
  {
    std::lock_guard lock{m_configMutex};
    m_pollConfig = sanitized;
    m_historyInterval = pollDuration(effectiveHistoryPollSeconds(sanitized));
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
  std::lock_guard lock{m_statsMutex};
  return m_latest;
}

std::vector<SystemStats> SystemMonitorService::history(int windowSize) const {
  std::lock_guard lock{m_statsMutex};
  return historyWindowFromRing(m_history, m_historyHead, windowSize);
}

void SystemMonitorService::retainCpuTemp() { m_cpuTempRefs.fetch_add(1, std::memory_order_relaxed); }

void SystemMonitorService::releaseCpuTemp() { m_cpuTempRefs.fetch_sub(1, std::memory_order_relaxed); }

void SystemMonitorService::retainGpuTemp() { m_gpuTempRefs.fetch_add(1, std::memory_order_relaxed); }

void SystemMonitorService::releaseGpuTemp() { m_gpuTempRefs.fetch_sub(1, std::memory_order_relaxed); }

void SystemMonitorService::retainGpuVram() { m_gpuVramRefs.fetch_add(1, std::memory_order_relaxed); }

void SystemMonitorService::releaseGpuVram() { m_gpuVramRefs.fetch_sub(1, std::memory_order_relaxed); }

void SystemMonitorService::retainDiskPath(const std::string& path) {
  const float initialPercent = isRunning() ? readDiskUsagePercent(path) : 0.0f;
  std::lock_guard lock{m_statsMutex};
  auto& disk = m_diskHistories[path];
  if (disk.refs == 0) {
    disk.latestPercent = initialPercent;
    disk.history.fill(initialPercent);
  }
  ++disk.refs;
}

void SystemMonitorService::releaseDiskPath(const std::string& path) {
  std::lock_guard lock{m_statsMutex};
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
  std::lock_guard lock{m_statsMutex};
  const auto it = m_diskHistories.find(path);
  return it != m_diskHistories.end() ? it->second.latestPercent : 0.0f;
}

std::vector<float> SystemMonitorService::diskHistory(const std::string& path, int windowSize) const {
  std::lock_guard lock{m_statsMutex};
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
  m_running = false;
  m_wakeCv.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void SystemMonitorService::logDetectedSources() {
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

  if (const auto cpuTemp = readCpuTempSensor(); cpuTemp.has_value()) {
    kLog.info("detected CPU temperature source: {} ({:.0f}C)", cpuTemp->source, cpuTemp->tempC);
  } else {
    kLog.info("detected CPU temperature source: unavailable");
  }

  const GpuHwmonProbe gpuHwmon = readGpuHwmonTempSensor();
  std::optional<TempSensorReading> gpuTemp = gpuHwmon.reading;
  std::string gpuDetail;

  if (gpuHwmon.foundNvidia) {
    gpuDetail = "NVIDIA hwmon present; NVML fallback not needed";
  } else if (hasInactiveNvidiaPciDisplayDevice()) {
    gpuDetail = "NVML skipped; NVIDIA display device is runtime-suspended";
  } else {
    if (m_nvidiaNvmlReader == nullptr) {
      m_nvidiaNvmlReader = std::make_unique<NvidiaNvmlReader>();
    }
    const auto nvml = m_nvidiaNvmlReader->readGpuTempSensor();
    if (nvml.has_value()) {
      gpuDetail = "NVML fallback available";
      if (!gpuTemp.has_value() || nvml->tempC > gpuTemp->tempC) {
        gpuTemp = nvml;
      }
    } else {
      gpuDetail = "NVML fallback unavailable";
    }
  }

  if (gpuTemp.has_value()) {
    kLog.info("detected GPU temperature source: {} ({:.0f}C); {}", gpuTemp->source, gpuTemp->tempC, gpuDetail);
  } else {
    kLog.info("detected GPU temperature source: unavailable; {}", gpuDetail);
  }

  if (const auto gpuVram = readGpuVram(); gpuVram.has_value()) {
    kLog.info(
        "detected GPU VRAM source: {} ({} / {})", gpuVram->source,
        FormatUnits::formatBinaryBytesAsGib(gpuVram->usedBytes),
        FormatUnits::formatBinaryBytesAsGib(gpuVram->totalBytes)
    );
  } else {
    kLog.info("detected GPU VRAM source: unavailable");
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
    const SystemConfig::MonitorConfig pollCfg = pollConfig();
    const auto cpuInterval = pollDuration(pollCfg.cpuPollSeconds);
    const auto gpuInterval = pollDuration(pollCfg.gpuPollSeconds);
    const auto memoryInterval = pollDuration(pollCfg.memoryPollSeconds);
    const auto networkInterval = pollDuration(pollCfg.networkPollSeconds);
    const auto diskInterval = pollDuration(pollCfg.diskPollSeconds);
    const auto historyInterval = pollDuration(effectiveHistoryPollSeconds(pollCfg));

    const auto now = Clock::now();
    bool statsTouched = false;

    if (now >= nextCpu) {
      const auto currentCpu = readCpuTotals();
      if (prevCpu.has_value() && currentCpu.has_value()) {
        const std::uint64_t totalDelta = currentCpu->total - prevCpu->total;
        const std::uint64_t idleDelta = currentCpu->idle - prevCpu->idle;
        if (totalDelta > 0) {
          std::lock_guard lock{m_statsMutex};
          m_latest.cpuUsagePercent = 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
        }
      }
      if (currentCpu.has_value()) {
        prevCpu = currentCpu;
      }

      if (const auto la = readLoadAvg(); la.has_value()) {
        std::lock_guard lock{m_statsMutex};
        m_latest.loadAvg1 = (*la)[0];
        m_latest.loadAvg5 = (*la)[1];
        m_latest.loadAvg15 = (*la)[2];
      }

      if (m_cpuTempRefs.load(std::memory_order_relaxed) > 0) {
        std::optional<double> cpuTemp = readCpuTempCelsius();
        std::lock_guard lock{m_statsMutex};
        if (cpuTemp.has_value()) {
          m_latest.cpuTempC = cpuTemp;
        } else if (!m_latest.cpuTempC.has_value()) {
          m_latest.cpuTempC = 40.0;
        }
      }

      nextCpu = now + cpuInterval;
      statsTouched = true;
    }

    if (now >= nextMemory) {
      if (const auto memKb = readMemoryKb(); memKb.has_value()) {
        std::lock_guard lock{m_statsMutex};
        m_latest.ramTotalMb = memKb->totalKb / 1024;
        m_latest.ramUsedMb = memKb->usedKb / 1024;
        if (memKb->totalKb > 0) {
          m_latest.ramUsagePercent = 100.0 * static_cast<double>(memKb->usedKb) / static_cast<double>(memKb->totalKb);
        }
      }
      nextMemory = now + memoryInterval;
      statsTouched = true;
    }

    if (now >= nextNetwork) {
      if (const auto currentNetBytes = readNetBytes(); currentNetBytes.has_value()) {
        const double intervalSeconds = std::chrono::duration<double>(networkInterval).count();
        const double scale = intervalSeconds > 0.0 ? 1.0 / intervalSeconds : 1.0;
        double totalRx = 0.0;
        double totalTx = 0.0;
        for (const auto& [iface, cur] : *currentNetBytes) {
          const auto it = m_prevNetBytes.find(iface);
          if (it != m_prevNetBytes.end()) {
            if (cur.rx >= it->second.rx) {
              totalRx += static_cast<double>(cur.rx - it->second.rx) * scale;
            }
            if (cur.tx >= it->second.tx) {
              totalTx += static_cast<double>(cur.tx - it->second.tx) * scale;
            }
          }
        }
        m_prevNetBytes = *currentNetBytes;
        std::lock_guard lock{m_statsMutex};
        m_latest.netRxBytesPerSec = totalRx;
        m_latest.netTxBytesPerSec = totalTx;
      }
      nextNetwork = now + networkInterval;
      statsTouched = true;
    }

    if (now >= nextGpu) {
      if (m_gpuTempRefs.load(std::memory_order_relaxed) > 0) {
        const auto gpuTemp = readGpuTempCelsius();
        std::lock_guard lock{m_statsMutex};
        if (gpuTemp.has_value()) {
          m_latest.gpuTempC = gpuTemp;
        }
      }
      if (m_gpuVramRefs.load(std::memory_order_relaxed) > 0) {
        if (const auto gpuVram = readGpuVram(); gpuVram.has_value()) {
          std::lock_guard lock{m_statsMutex};
          m_latest.gpuVramUsedBytes = gpuVram->usedBytes;
          m_latest.gpuVramTotalBytes = gpuVram->totalBytes;
        }
      }
      nextGpu = now + gpuInterval;
      statsTouched = true;
    }

    if (now >= nextDisk) {
      if (const auto memKb = readMemoryKb(); memKb.has_value()) {
        std::lock_guard lock{m_statsMutex};
        m_latest.swapTotalMb = memKb->swapTotalKb / 1024;
        m_latest.swapUsedMb = memKb->swapUsedKb / 1024;
      }
      std::vector<std::string> diskPaths;
      {
        std::lock_guard lock{m_statsMutex};
        diskPaths.reserve(m_diskHistories.size());
        for (const auto& [path, disk] : m_diskHistories) {
          if (disk.refs > 0) {
            diskPaths.push_back(path);
          }
        }
      }
      for (const auto& path : diskPaths) {
        const float percent = readDiskUsagePercent(path);
        std::lock_guard lock{m_statsMutex};
        const auto it = m_diskHistories.find(path);
        if (it != m_diskHistories.end() && it->second.refs > 0) {
          it->second.latestPercent = percent;
        }
      }
      nextDisk = now + diskInterval;
    }

    if (statsTouched) {
      std::lock_guard lock{m_statsMutex};
      m_latest.sampledAt = now;
    }

    if (now >= nextHistory) {
      std::lock_guard lock{m_statsMutex};
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

    const auto nextWake = std::min({nextCpu, nextGpu, nextMemory, nextNetwork, nextDisk, nextHistory});
    std::unique_lock wakeLock{m_wakeMutex};
    m_wakeCv.wait_until(wakeLock, nextWake, [this]() { return !m_running.load(); });
  }
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

  MemData data;
  data.totalKb = totalKb;
  data.usedKb = totalKb - availableKb;
  data.swapTotalKb = swapTotalKb;
  data.swapUsedKb = swapTotalKb > swapFreeKb ? swapTotalKb - swapFreeKb : 0;
  return data;
}

std::optional<double> SystemMonitorService::readCpuTempCelsius() {
  const auto reading = readCpuTempSensor();
  return reading.has_value() ? std::optional<double>{reading->tempC} : std::nullopt;
}

std::optional<double> SystemMonitorService::readGpuTempCelsius() {
  const GpuHwmonProbe hwmon = readGpuHwmonTempSensor();
  if (hwmon.reading.has_value() && hwmon.foundNvidia) {
    return hwmon.reading->tempC;
  }

  std::optional<TempSensorReading> best = hwmon.reading;
  if (!hasInactiveNvidiaPciDisplayDevice() && !hwmon.foundNvidia) {
    if (m_nvidiaNvmlReader == nullptr) {
      m_nvidiaNvmlReader = std::make_unique<NvidiaNvmlReader>();
    }
    const auto nvml = m_nvidiaNvmlReader->readGpuTempSensor();
    if (nvml.has_value() && (!best.has_value() || nvml->tempC > best->tempC)) {
      best = nvml;
    }
  }

  return best.has_value() ? std::optional<double>{best->tempC} : std::nullopt;
}

std::optional<SystemMonitorService::GpuVramData> SystemMonitorService::readGpuVram() {
  std::optional<GpuVramReading> combined = readAmdGpuVram();

  if (!hasInactiveNvidiaPciDisplayDevice()) {
    if (m_nvidiaNvmlReader == nullptr) {
      m_nvidiaNvmlReader = std::make_unique<NvidiaNvmlReader>();
    }
    const auto nvml = m_nvidiaNvmlReader->readGpuVram();
    if (nvml.has_value()) {
      if (combined.has_value()) {
        mergeGpuVram(*combined, *nvml);
      } else {
        combined = nvml;
      }
    }
  }

  if (!combined.has_value() || !hasUsableVram(*combined)) {
    return std::nullopt;
  }
  return GpuVramData{.usedBytes = combined->usedBytes, .totalBytes = combined->totalBytes, .source = combined->source};
}

float SystemMonitorService::readDiskUsagePercent(const std::string& path) {
  struct statvfs sv{};
  if (::statvfs(path.c_str(), &sv) == 0 && sv.f_blocks > 0) {
    const double used = static_cast<double>(sv.f_blocks - sv.f_bfree);
    const double total = static_cast<double>(sv.f_blocks);
    return static_cast<float>(100.0 * used / total);
  }
  return 0.0f;
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
    if (iface == "lo") {
      continue;
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
