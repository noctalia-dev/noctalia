#pragma once

#include "config/config_types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

struct SystemStats {
  struct NetThroughput {
    double rxBytesPerSec{0.0};
    double txBytesPerSec{0.0};
  };

  std::chrono::steady_clock::time_point sampledAt;
  double cpuUsagePercent{0.0};
  // Per-core usage, indexed by the /proc/stat "cpuN" order. Empty unless a consumer
  // has called retainCpuCores(); sampled on its own fixed 1s cadence.
  std::vector<double> cpuCoreUsagePercent;
  double ramUsagePercent{0.0};
  std::uint64_t ramUsedMb{0};
  std::uint64_t ramTotalMb{0};
  std::uint64_t swapUsedMb{0};
  std::uint64_t swapTotalMb{0};
  std::optional<double> cpuTempC;
  bool cpuTempAvailable{false};
  std::optional<double> gpuTempC;
  std::optional<double> gpuUsagePercent;
  std::optional<std::uint64_t> gpuVramUsedBytes;
  std::optional<std::uint64_t> gpuVramTotalBytes;
  double netRxBytesPerSec{0.0};
  double netTxBytesPerSec{0.0};
  std::unordered_map<std::string, NetThroughput> netThroughputByInterface;
  double loadAvg1{0.0};
  double loadAvg5{0.0};
  double loadAvg15{0.0};
};

class SystemMonitorService {
public:
  explicit SystemMonitorService(const SystemConfig::MonitorConfig& config = {});
  ~SystemMonitorService();

  SystemMonitorService(const SystemMonitorService&) = delete;
  SystemMonitorService& operator=(const SystemMonitorService&) = delete;

  static constexpr int kHistorySize = 120;

  [[nodiscard]] bool isRunning() const noexcept;
  void applyConfig(const SystemConfig::MonitorConfig& config);
  void setEnabled(bool enabled);
  [[nodiscard]] SystemStats latest() const;
  [[nodiscard]] std::vector<SystemStats> history(int windowSize = kHistorySize) const;
  [[nodiscard]] std::chrono::steady_clock::duration historySampleInterval() const noexcept;
  [[nodiscard]] double netRxBytesPerSec(std::string_view interfaceName = {}) const;
  [[nodiscard]] double netTxBytesPerSec(std::string_view interfaceName = {}) const;

  void retainCpuTemp();
  void releaseCpuTemp();
  void retainCpuCores();
  void releaseCpuCores();
  void retainGpuTemp();
  void releaseGpuTemp();
  void retainGpuUsage();
  void releaseGpuUsage();
  void retainGpuVram();
  void releaseGpuVram();
  void retainDiskPath(const std::string& path);
  void releaseDiskPath(const std::string& path);
  [[nodiscard]] float diskUsagePercent(const std::string& path) const;
  [[nodiscard]] std::vector<float> diskHistory(const std::string& path, int windowSize = kHistorySize) const;

private:
  struct NvidiaNvmlReader;
  struct AmdRsmiReader;
  struct IntelGpuReader;

  struct DiskHistory {
    int refs = 0;
    float latestPercent = 0.0f;
    std::array<float, kHistorySize> history{};
  };

  struct CpuTotals {
    std::uint64_t total{0};
    std::uint64_t idle{0};
  };

  struct GpuVramData {
    std::uint64_t usedBytes{0};
    std::uint64_t totalBytes{0};
    std::string source;
  };

  enum class NvidiaDisplayDeviceState { None, InactiveOnly, Active };

  struct GpuTempData {
    std::optional<double> tempC;
    std::string source;
    std::string detail;
  };

  struct GpuUsageData {
    std::optional<double> percent;
    std::string source;
  };

  void start();
  void stop();
  void samplingLoop();
  void logDetectedSources();

  // Parses one "/proc/stat" cpu row into idle/total jiffies. `expectedLabel` pins which row is
  // accepted ("cpu" for the aggregate, "cpuN" for a core), so a caller cannot mistake the
  // aggregate for core 0. guest/guest_nice are deliberately not read: the kernel already folds
  // them into user/nice.
  [[nodiscard]] static std::optional<CpuTotals>
  parseCpuStatLine(const std::string& line, std::string_view expectedLabel);
  // Busy percentage between two samples, or nullopt when the window contains no jiffies.
  [[nodiscard]] static std::optional<double> cpuUsageBetween(const CpuTotals& prev, const CpuTotals& current);
  [[nodiscard]] static std::optional<CpuTotals> readCpuTotals();
  // Per-core totals in /proc/stat order, skipping the leading aggregate "cpu" row.
  [[nodiscard]] static std::optional<std::vector<CpuTotals>> readCpuCoreTotals();
  struct MemData {
    std::uint64_t totalKb{0};
    std::uint64_t usedKb{0};
    std::uint64_t swapTotalKb{0};
    std::uint64_t swapUsedKb{0};
  };
  [[nodiscard]] static std::optional<MemData> readMemoryKb();
  [[nodiscard]] static std::optional<double> readCpuTempCelsius(const SystemConfig::MonitorConfig& config);
  [[nodiscard]] static NvidiaDisplayDeviceState detectNvidiaPciDisplayDeviceState();
  [[nodiscard]] NvidiaNvmlReader& ensureNvmlReader();
  [[nodiscard]] AmdRsmiReader& ensureAmdRsmiReader();
  [[nodiscard]] IntelGpuReader& ensureIntelGpuReader();
  [[nodiscard]] GpuTempData readGpuTempData(NvidiaDisplayDeviceState nvidiaDisplayState);
  [[nodiscard]] GpuUsageData readGpuUsageData(NvidiaDisplayDeviceState nvidiaDisplayState);
  [[nodiscard]] GpuUsageData readIntelGpuUsageData();
  [[nodiscard]] std::optional<GpuVramData> readIntelGpuVram();
  [[nodiscard]] std::optional<GpuVramData> readGpuVramData(NvidiaDisplayDeviceState nvidiaDisplayState);
  [[nodiscard]] std::optional<double> readGpuTempCelsius();
  [[nodiscard]] std::optional<double> readGpuUsagePercent();
  [[nodiscard]] std::optional<GpuVramData> readGpuVram();
  [[nodiscard]] static float readDiskUsagePercent(const std::string& path);

  struct NetIfaceBytes {
    std::uint64_t rx{0};
    std::uint64_t tx{0};
  };
  [[nodiscard]] static std::optional<std::unordered_map<std::string, NetIfaceBytes>> readNetBytes();
  [[nodiscard]] static std::optional<std::array<double, 3>> readLoadAvg();

  [[nodiscard]] SystemConfig::MonitorConfig pollConfig() const;

  std::atomic<bool> m_running{false};
  std::atomic<int> m_cpuTempRefs{0};
  std::atomic<int> m_cpuCoreRefs{0};
  std::atomic<int> m_gpuTempRefs{0};
  std::atomic<int> m_gpuUsageRefs{0};
  std::atomic<int> m_gpuVramRefs{0};
  std::thread m_thread;
  std::mutex m_wakeMutex;
  std::condition_variable m_wakeCv;

  mutable std::mutex m_configMutex;
  SystemConfig::MonitorConfig m_pollConfig;
  std::chrono::steady_clock::duration m_historyInterval{std::chrono::seconds(1)};

  mutable std::mutex m_statsMutex;
  SystemStats m_latest;
  std::array<SystemStats, kHistorySize> m_history{};
  int m_historyHead = 0;
  std::unordered_map<std::string, DiskHistory> m_diskHistories;
  std::unordered_map<std::string, NetIfaceBytes> m_prevNetBytes;
  std::unique_ptr<NvidiaNvmlReader> m_nvidiaNvmlReader;
  std::unique_ptr<AmdRsmiReader> m_amdRsmiReader;
  std::unique_ptr<IntelGpuReader> m_intelGpuReader;
};
