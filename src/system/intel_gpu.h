#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace noctalia::system::intel_gpu {

  enum class Driver { I915, Xe };

  struct Device {
    std::filesystem::path devicePath;
    std::filesystem::path renderNode;
    std::string pciSlot;
    Driver driver = Driver::Xe;
  };

  struct VramReading {
    std::uint64_t usedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::string source;
  };

  struct UsageReading {
    double percent = 0.0;
    std::string source;
  };

  // Intel GPUs bound to i915 or xe. Discovery is by PCI vendor 0x8086 plus the bound driver name,
  // so a card with no driver loaded is not reported.
  [[nodiscard]] std::vector<Device> findDevices(const std::filesystem::path& drmRoot = "/sys/class/drm");

  [[nodiscard]] std::string usageSource(const Device& device);

  // Discrete VRAM via the xe DRM device-query ioctl on the render node. Integrated GPUs have no
  // VRAM memory region and report nothing. i915 never reports VRAM: its used-size query is gated
  // behind CAP_PERFMON on every kernel, and an unprivileged caller only ever sees zero.
  //
  // xe kernels before 6.18 gate the used size the same way; a zero used size is reported as
  // unavailable rather than as an idle GPU.
  [[nodiscard]] std::optional<VramReading> readVram(const Device& device);

  // Engine busyness is only exposed per DRM client, so utilization is the sum over every client's
  // counter delta between two scans of /proc/<pid>/fdinfo. The sampler holds the previous scan and
  // returns nothing until it has two.
  class UsageSampler {
  public:
    [[nodiscard]] std::optional<UsageReading> sample(const Device& device);
    [[nodiscard]] bool hasBaseline() const noexcept { return m_hasBaseline; }

  private:
    // Render and compute share the execution units on Intel, so they are the two classes that
    // define overall load. Media engines are tracked by neither.
    static constexpr std::size_t kEngineClassCount = 2;

    struct EngineCounters {
      std::array<std::uint64_t, kEngineClassCount> busy{};
    };

    std::unordered_map<std::uint64_t, EngineCounters> m_clients;
    std::array<std::uint64_t, kEngineClassCount> m_gpuTicks{};
    std::chrono::steady_clock::time_point m_sampledAt;
    bool m_hasBaseline = false;
  };

} // namespace noctalia::system::intel_gpu
