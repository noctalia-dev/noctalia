#include "scripting/luau_host.h"
#include "scripting/script_api_context.h"
#include "system/system_monitor_service.h"
#include "tests/test_check.h"

#include <array>
#include <cstdint>

// Observe the real service's subscription state without loading GPU drivers. Polling is disabled
// throughout this test, so it is safe and deterministic on both hybrid laptops and GPU-less CI.
class SystemMonitorServiceTestAccess {
public:
  static std::array<int, 3> gpuRefs(const SystemMonitorService& monitor) {
    return {monitor.m_gpuTempRefs.load(), monitor.m_gpuUsageRefs.load(), monitor.m_gpuVramRefs.load()};
  }

  static int cpuTempRefs(const SystemMonitorService& monitor) { return monitor.m_cpuTempRefs.load(); }

  static std::uint64_t wakeGeneration(const SystemMonitorService& monitor) { return monitor.m_wakeGeneration.load(); }
};

int main() {
  SystemConfig::MonitorConfig config;
  config.cpuPollSeconds = 0.0F;
  config.gpuPollSeconds = 0.0F;
  config.memoryPollSeconds = 0.0F;
  config.networkPollSeconds = 0.0F;
  config.diskPollSeconds = 0.0F;
  SystemMonitorService monitor(config);
  scripting::ScriptApiContext api;
  api.setSystemMonitor(&monitor);

  const auto expectRefs = [&](int temp, int usage, int vram) {
    const std::array expected{temp, usage, vram};
    TEST_CHECK(SystemMonitorServiceTestAccess::gpuRefs(monitor) == expected);
  };

  {
    LuauHost cpuOnly(api, "test/cpu:widget");
    TEST_CHECK(cpuOnly.exec("=cpu-only", R"(
      for i = 1, 3 do
        local stats = noctalia.systemStats()
        assert(type(stats.cpu.usagePercent) == 'number')
        assert(type(stats.ram.usedMb) == 'number')
        assert(type(stats.net.rxBytesPerSec) == 'number')
        assert(type(stats.gpu) == 'table')
      end
    )"));
    expectRefs(0, 0, 0);
    TEST_CHECK(SystemMonitorServiceTestAccess::cpuTempRefs(monitor) == 1);

    // A native widget and two independent Lua hosts must not release one another's requests.
    monitor.retainGpuUsage();
    {
      LuauHost panel(api, "test/gpu:panel");
      TEST_CHECK(panel.exec("=panel", R"(
        function onOpen() assert(noctalia.setGpuMonitoring(true)) end
        function onClose() assert(noctalia.setGpuMonitoring(false)) end
        onOpen()
        onOpen()
        assert(type(noctalia.systemStats().gpu) == 'table')
      )"));
      expectRefs(1, 2, 1);
      {
        LuauHost second(api, "test/gpu:widget");
        TEST_CHECK(second.exec("=second", "assert(noctalia.setGpuMonitoring(true))"));
        expectRefs(2, 3, 2);
        TEST_CHECK(panel.exec("=close", "onClose(); onClose()"));
        expectRefs(1, 2, 1);
        TEST_CHECK(panel.exec("=closed-snapshot", "noctalia.systemStats()"));
        expectRefs(1, 2, 1);
        TEST_CHECK(panel.exec("=reopen", "onOpen()"));
        expectRefs(2, 3, 2);
      }
      expectRefs(1, 2, 1);

      // A malformed request cannot accidentally enable monitoring (Lua strings are truthy).
      TEST_CHECK(panel.exec("=invalid", R"(
        onClose()
        assert(not pcall(noctalia.setGpuMonitoring, 'false'))
        assert(not pcall(noctalia.setGpuMonitoring))
        assert(not pcall(noctalia.setGpuMonitoring, 1))
      )"));
      expectRefs(0, 1, 0);
      const auto wake = SystemMonitorServiceTestAccess::wakeGeneration(monitor);
      TEST_CHECK(panel.exec("=release-last", "onOpen(); onClose()"));
      expectRefs(0, 1, 0);
      TEST_CHECK(SystemMonitorServiceTestAccess::wakeGeneration(monitor) > wake);

      // Requests still balance across a disabled service and are cleaned up on host destruction.
      TEST_CHECK(panel.exec("=before-stop", "onOpen()"));
      monitor.setEnabled(false);
      TEST_CHECK(panel.exec("=disabled", R"(
        assert(noctalia.systemStats() == nil)
        onClose()
      )"));
      expectRefs(0, 1, 0);
      monitor.setEnabled(true);
      TEST_CHECK(panel.exec("=after-start", "onOpen()"));
      expectRefs(1, 2, 1);
    }
    expectRefs(0, 1, 0);
    TEST_CHECK(SystemMonitorServiceTestAccess::cpuTempRefs(monitor) == 1);
    monitor.releaseGpuUsage();
    expectRefs(0, 0, 0);
  }
  TEST_CHECK(SystemMonitorServiceTestAccess::cpuTempRefs(monitor) == 0);
  expectRefs(0, 0, 0);

  // A failed request must be retryable once the monitor becomes available.
  api.setSystemMonitor(nullptr);
  {
    LuauHost host(api, "test/unavailable:widget");
    TEST_CHECK(host.exec("=unavailable", R"(
      assert(noctalia.systemStats() == nil)
      assert(not noctalia.setGpuMonitoring(true))
      assert(noctalia.setGpuMonitoring(false))
    )"));
    api.setSystemMonitor(&monitor);
    TEST_CHECK(host.exec("=retry", "assert(noctalia.setGpuMonitoring(true))"));
    expectRefs(1, 1, 1);
  }
  expectRefs(0, 0, 0);
  api.setSystemMonitor(nullptr);
  return 0;
}
