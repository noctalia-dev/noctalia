#include "config/config_types.h"
#include "system/system_monitor_service.h"

#include <chrono>
#include <filesystem>
#include <print>
#include <string>
#include <thread>

namespace {

  int g_failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "system_monitor_service_test: FAIL: {}", message);
      ++g_failures;
    }
  }

  void testDiskSnapshot(SystemMonitorService& monitor) {
    const std::string path = std::filesystem::temp_directory_path().string();
    monitor.retainDiskPath(path);

    const auto disk = monitor.diskStats(path);
    expect(disk.has_value(), "a valid path should produce a disk snapshot");
    if (disk.has_value()) {
      expect(disk->totalBytes > 0, "disk total should be populated");
      expect(disk->freeBytes <= disk->totalBytes, "disk free bytes should not exceed the total");
      expect(disk->availableBytes <= disk->freeBytes, "available bytes should not exceed free bytes");
      expect(disk->usagePercent >= 0.0F && disk->usagePercent <= 100.0F, "disk usage should be a percentage");
    }
    monitor.releaseDiskPath(path);

    const std::string missingPath = path + "/definitely-not-a-noctalia-filesystem";
    monitor.retainDiskPath(missingPath);
    expect(!monitor.diskStats(missingPath).has_value(), "an unavailable path should not produce a zero snapshot");
    monitor.releaseDiskPath(missingPath);
  }

  void testSampleTimestamp(SystemMonitorService& monitor) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    SystemStats stats;
    do {
      stats = monitor.latest();
      if (stats.sampledAtWall != std::chrono::system_clock::time_point{}) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);

    expect(stats.sampledAtWall != std::chrono::system_clock::time_point{}, "a completed sample should carry wall time");
    expect(stats.sampledAtWall <= std::chrono::system_clock::now(), "sample wall time should not be in the future");
  }

  void testGpuMonitoring(SystemMonitorService& monitor) {
    monitor.retainGpuUsage();
    monitor.retainGpuTemp();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
      const auto stats = monitor.latest();
      if (stats.gpuUsagePercent.has_value() || stats.gpuTempC.has_value()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);

    const auto stats = monitor.latest();
    if (stats.gpuUsagePercent.has_value()) {
      expect(*stats.gpuUsagePercent >= 0.0 && *stats.gpuUsagePercent <= 100.0, "gpu usage should be between 0 and 100");
    }
    if (stats.gpuTempC.has_value()) {
      expect(*stats.gpuTempC >= -50.0 && *stats.gpuTempC <= 150.0, "gpu temp should be in a plausible range");
    }
    monitor.releaseGpuUsage();
    monitor.releaseGpuTemp();
  }

} // namespace

int main() {
  SystemConfig::MonitorConfig config;
  config.cpuPollSeconds = 1.0F;
  config.gpuPollSeconds = 1.0F;
  config.memoryPollSeconds = 0.0F;
  config.networkPollSeconds = 0.0F;
  config.diskPollSeconds = 1.0F;

  SystemMonitorService monitor(config);
  testDiskSnapshot(monitor);
  testSampleTimestamp(monitor);
  testGpuMonitoring(monitor);
  return g_failures == 0 ? 0 : 1;
}
