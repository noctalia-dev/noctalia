#include "config/config_types.h"
#include "system/system_monitor_service.h"
#include "tests/test_check.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

  void testDiskSnapshot(SystemMonitorService& monitor) {
    const std::string path = std::filesystem::temp_directory_path().string();
    monitor.retainDiskPath(path);

    const auto disk = monitor.diskStats(path);
    TEST_CHECK(disk.has_value());
    TEST_CHECK(disk->totalBytes > 0);
    TEST_CHECK(disk->freeBytes <= disk->totalBytes);
    TEST_CHECK(disk->availableBytes <= disk->freeBytes);
    TEST_CHECK(disk->usagePercent >= 0.0F && disk->usagePercent <= 100.0F);
    monitor.releaseDiskPath(path);

    const std::string missingPath = path + "/definitely-not-a-noctalia-filesystem";
    monitor.retainDiskPath(missingPath);
    // an unavailable path should not produce a zero snapshot
    TEST_CHECK(!monitor.diskStats(missingPath).has_value());
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

    TEST_CHECK(stats.sampledAtWall != std::chrono::system_clock::time_point{});
    TEST_CHECK(stats.sampledAtWall <= std::chrono::system_clock::now());
  }

} // namespace

int main() {
  SystemConfig::MonitorConfig config;
  config.cpuPollSeconds = 1.0F;
  config.gpuPollSeconds = 0.0F;
  config.memoryPollSeconds = 0.0F;
  config.networkPollSeconds = 0.0F;
  config.diskPollSeconds = 1.0F;

  SystemMonitorService monitor(config);
  testDiskSnapshot(monitor);
  testSampleTimestamp(monitor);
  return 0;
}
