#include "system/intel_gpu.h"

#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>

namespace noctalia::system::intel_gpu {

  namespace {

    constexpr std::string_view kIntelPciVendor = "0x8086";
    constexpr std::string_view kDrmNodeDir = "/dev/dri/";

    // uapi/drm/xe_drm.h. Mirrored here so the build does not depend on a libdrm new enough to ship
    // xe_drm.h; the layouts are frozen uapi and the static_asserts below pin them.
    constexpr unsigned int kDrmCommandBase = 0x40;
    constexpr unsigned int kDrmXeDeviceQuery = 0x00;
    constexpr unsigned int kDrmXeQueryMemRegions = 1;
    constexpr std::uint16_t kDrmXeMemRegionClassVram = 1;

    struct DrmXeDeviceQuery {
      std::uint64_t extensions;
      std::uint32_t query;
      std::uint32_t size;
      std::uint64_t data;
      std::array<std::uint64_t, 2> reserved;
    };

    struct DrmXeMemRegion {
      std::uint16_t memClass;
      std::uint16_t instance;
      std::uint32_t minPageSize;
      std::uint64_t totalSize;
      std::uint64_t used;
      std::uint64_t cpuVisibleSize;
      std::uint64_t cpuVisibleUsed;
      std::array<std::uint64_t, 6> reserved;
    };

    struct DrmXeQueryMemRegions {
      std::uint32_t numMemRegions;
      std::uint32_t pad;
    };

    static_assert(sizeof(DrmXeDeviceQuery) == 40);
    static_assert(sizeof(DrmXeMemRegion) == 88);
    static_assert(sizeof(DrmXeQueryMemRegions) == 8);

    // Engine class keys, indexed to match UsageSampler's render/compute slots.
    struct EngineKeys {
      std::string_view busy;
      std::string_view gpuTicks;
      std::string_view capacity;
    };

    // xe reports engine time in GPU cycles alongside a free-running GPU timestamp, so its
    // utilization needs no wall clock. i915 reports nanoseconds and has no timestamp key.
    constexpr std::array<EngineKeys, 2> kXeEngineKeys{
        EngineKeys{"drm-cycles-rcs", "drm-total-cycles-rcs", "drm-engine-capacity-rcs"},
        EngineKeys{"drm-cycles-ccs", "drm-total-cycles-ccs", "drm-engine-capacity-ccs"},
    };

    constexpr std::array<EngineKeys, 2> kI915EngineKeys{
        EngineKeys{"drm-engine-render", "", "drm-engine-capacity-render"},
        EngineKeys{"drm-engine-compute", "", "drm-engine-capacity-compute"},
    };

    [[nodiscard]] const std::array<EngineKeys, 2>& engineKeysFor(Driver driver) {
      return driver == Driver::Xe ? kXeEngineKeys : kI915EngineKeys;
    }

    [[nodiscard]] std::string_view driverName(Driver driver) { return driver == Driver::Xe ? "xe" : "i915"; }

    [[nodiscard]] bool isDrmCardName(std::string_view name) {
      return name.starts_with("card")
          && name.size() > 4
          && std::ranges::all_of(name.substr(4), [](char ch) { return ch >= '0' && ch <= '9'; });
    }

    [[nodiscard]] bool isPidName(std::string_view name) {
      return !name.empty() && std::ranges::all_of(name, [](char ch) { return ch >= '0' && ch <= '9'; });
    }

    // fdinfo values are "key:\tvalue"; i915 suffixes engine times with " ns", so only the leading
    // integer is parsed.
    [[nodiscard]] std::optional<std::uint64_t> parseLeadingUint(std::string_view value) {
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
      }
      std::uint64_t parsed = 0;
      const auto* const end = value.data() + value.size();
      const auto result = std::from_chars(value.data(), end, parsed);
      if (result.ec != std::errc{}) {
        return std::nullopt;
      }
      return parsed;
    }

    // The render node is the unprivileged entry point for the device query; the card node would
    // need the seat ACL and DRM master.
    [[nodiscard]] std::filesystem::path findRenderNode(const std::filesystem::path& devicePath) {
      namespace fs = std::filesystem;

      std::error_code ec;
      const fs::path drmDir = devicePath / "drm";
      if (!fs::is_directory(drmDir, ec)) {
        return {};
      }

      for (const auto& entry : fs::directory_iterator{drmDir, ec}) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("renderD")) {
          return fs::path{kDrmNodeDir} / name;
        }
      }
      return {};
    }

    struct FdinfoClient {
      std::uint64_t clientId = 0;
      std::array<std::uint64_t, 2> busy{};
      std::array<std::uint64_t, 2> gpuTicks{};
      std::array<std::uint64_t, 2> capacity{1, 1};
    };

    [[nodiscard]] std::optional<FdinfoClient>
    parseFdinfo(const std::filesystem::path& path, const Device& device, const std::array<EngineKeys, 2>& keys) {
      std::ifstream file{path};
      if (!file.is_open()) {
        return std::nullopt;
      }

      FdinfoClient client;
      bool matchedDriver = false;
      bool matchedDevice = false;
      bool haveClientId = false;

      std::string line;
      while (std::getline(file, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
          continue;
        }

        const std::string_view key{line.data(), colon};
        std::string_view value{line};
        value.remove_prefix(colon + 1);

        if (key == "drm-driver") {
          matchedDriver = value.contains(driverName(device.driver));
          continue;
        }
        if (key == "drm-pdev") {
          matchedDevice = value.contains(device.pciSlot);
          continue;
        }
        if (key == "drm-client-id") {
          if (const auto parsed = parseLeadingUint(value); parsed.has_value()) {
            client.clientId = *parsed;
            haveClientId = true;
          }
          continue;
        }

        const auto parsed = parseLeadingUint(value);
        if (!parsed.has_value()) {
          continue;
        }

        for (std::size_t engine = 0; engine < keys.size(); ++engine) {
          if (key == keys[engine].busy) {
            client.busy[engine] = *parsed;
          } else if (!keys[engine].gpuTicks.empty() && key == keys[engine].gpuTicks) {
            client.gpuTicks[engine] = *parsed;
          } else if (key == keys[engine].capacity && *parsed > 0) {
            client.capacity[engine] = *parsed;
          }
        }
      }

      if (!matchedDriver || !matchedDevice || !haveClientId) {
        return std::nullopt;
      }
      return client;
    }

    // Every open of the DRM device is one drm_file with one client id. The same id shows up under
    // every fd that dup()'d or inherited it, in any process, so the id is the dedup key.
    void collectFdinfoClients(const Device& device, std::unordered_map<std::uint64_t, FdinfoClient>& out) {
      namespace fs = std::filesystem;

      const auto& keys = engineKeysFor(device.driver);
      std::error_code ec;
      for (const auto& procEntry : fs::directory_iterator{"/proc", ec}) {
        const std::string pid = procEntry.path().filename().string();
        if (!isPidName(pid)) {
          continue;
        }

        // fdinfo is PTRACE_MODE_READ gated: other users' processes are unreadable and skipped.
        // On a single-user session every GPU client is the user's own.
        const fs::path fdDir = procEntry.path() / "fd";
        std::error_code dirEc;
        for (const auto& fdEntry : fs::directory_iterator{fdDir, dirEc}) {
          std::error_code linkEc;
          const fs::path target = fs::read_symlink(fdEntry.path(), linkEc);
          if (linkEc || !target.string().starts_with(kDrmNodeDir)) {
            continue;
          }

          const fs::path fdinfoPath = procEntry.path() / "fdinfo" / fdEntry.path().filename();
          if (const auto client = parseFdinfo(fdinfoPath, device, keys); client.has_value()) {
            out.insert_or_assign(client->clientId, *client);
          }
        }
      }
    }

    // The DRM usage-stats contract allows a counter to regress; the larger previous value stands
    // until a monotonic update arrives.
    [[nodiscard]] std::uint64_t monotonicDelta(std::uint64_t current, std::uint64_t previous) {
      return current > previous ? current - previous : 0;
    }

  } // namespace

  std::vector<Device> findDevices(const std::filesystem::path& drmRoot) {
    namespace fs = std::filesystem;

    std::vector<Device> devices;
    std::error_code ec;
    if (!fs::is_directory(drmRoot, ec)) {
      return devices;
    }

    for (const auto& entry : fs::directory_iterator{drmRoot, ec}) {
      if (!isDrmCardName(entry.path().filename().string())) {
        continue;
      }

      const fs::path devicePath = entry.path() / "device";
      if (FileUtils::readSmallTextFile(devicePath / "vendor").value_or("") != kIntelPciVendor) {
        continue;
      }

      std::error_code linkEc;
      const fs::path driverLink = fs::read_symlink(devicePath / "driver", linkEc);
      if (linkEc) {
        continue;
      }

      const std::string driverBase = driverLink.filename().string();
      Device device;
      if (driverBase == "xe") {
        device.driver = Driver::Xe;
      } else if (driverBase == "i915") {
        device.driver = Driver::I915;
      } else {
        continue;
      }

      // The PCI slot is the fdinfo drm-pdev string.
      const fs::path deviceLink = fs::read_symlink(entry.path() / "device", linkEc);
      if (linkEc) {
        continue;
      }

      device.devicePath = devicePath;
      device.pciSlot = deviceLink.filename().string();
      device.renderNode = findRenderNode(devicePath);
      devices.push_back(std::move(device));
    }

    return devices;
  }

  std::string usageSource(const Device& device) {
    return std::format("{} fdinfo:{}", driverName(device.driver), device.pciSlot);
  }

  std::optional<VramReading> readVram(const Device& device) {
    if (device.driver != Driver::Xe || device.renderNode.empty()) {
      return std::nullopt;
    }

    const int fd = ::open(device.renderNode.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      return std::nullopt;
    }

    const unsigned long request = _IOWR('d', kDrmCommandBase + kDrmXeDeviceQuery, DrmXeDeviceQuery);

    DrmXeDeviceQuery query{};
    query.query = kDrmXeQueryMemRegions;
    if (::ioctl(fd, request, &query) != 0 || query.size < sizeof(DrmXeQueryMemRegions)) {
      ::close(fd);
      return std::nullopt;
    }

    // uint64 storage guarantees the 8-byte alignment the region structs need.
    std::vector<std::uint64_t> buffer((query.size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0);
    query.data = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buffer.data()));
    if (::ioctl(fd, request, &query) != 0) {
      ::close(fd);
      return std::nullopt;
    }
    ::close(fd);

    DrmXeQueryMemRegions header{};
    std::memcpy(&header, buffer.data(), sizeof(header));

    const auto* const bytes = reinterpret_cast<const std::byte*>(buffer.data());
    for (std::uint32_t i = 0; i < header.numMemRegions; ++i) {
      const std::size_t offset = sizeof(DrmXeQueryMemRegions) + (std::size_t{i} * sizeof(DrmXeMemRegion));
      if (offset + sizeof(DrmXeMemRegion) > query.size) {
        break;
      }

      DrmXeMemRegion region{};
      std::memcpy(&region, bytes + offset, sizeof(region));
      if (region.memClass != kDrmXeMemRegionClassVram || region.totalSize == 0) {
        continue;
      }

      // Kernels that still gate the used size behind CAP_PERFMON report zero; an idle GPU never
      // does, so zero means the kernel is withholding it.
      if (region.used == 0 || region.used > region.totalSize) {
        return std::nullopt;
      }

      return VramReading{
          .usedBytes = region.used,
          .totalBytes = region.totalSize,
          .source = std::format("xe drm query:{}", device.pciSlot),
      };
    }

    return std::nullopt;
  }

  std::optional<UsageReading> UsageSampler::sample(const Device& device) {
    std::unordered_map<std::uint64_t, FdinfoClient> clients;
    collectFdinfoClients(device, clients);

    const auto now = std::chrono::steady_clock::now();

    std::array<std::uint64_t, kEngineClassCount> busyDelta{};
    std::array<std::uint64_t, kEngineClassCount> gpuTicks{};
    std::array<std::uint64_t, kEngineClassCount> capacity{1, 1};

    std::unordered_map<std::uint64_t, EngineCounters> current;
    current.reserve(clients.size());

    for (const auto& [clientId, client] : clients) {
      EngineCounters counters;
      const auto previous = m_clients.find(clientId);

      for (std::size_t engine = 0; engine < kEngineClassCount; ++engine) {
        counters.busy[engine] = client.busy[engine];
        capacity[engine] = std::max(capacity[engine], client.capacity[engine]);

        // Every client on a device reports the same GPU timestamp, each sampled at its own
        // instant; the newest is the device's.
        gpuTicks[engine] = std::max(gpuTicks[engine], client.gpuTicks[engine]);

        // A client first seen this scan only establishes a baseline — its counter already holds
        // work done before the sampler existed.
        if (previous != m_clients.end()) {
          busyDelta[engine] += monotonicDelta(client.busy[engine], previous->second.busy[engine]);
        }
      }

      current.insert_or_assign(clientId, counters);
    }

    const bool hadBaseline = m_hasBaseline;
    const auto previousTicks = m_gpuTicks;
    const auto previousAt = m_sampledAt;

    m_clients = std::move(current);
    m_gpuTicks = gpuTicks;
    m_sampledAt = now;
    m_hasBaseline = true;

    if (!hadBaseline) {
      return std::nullopt;
    }

    double percent = 0.0;
    for (std::size_t engine = 0; engine < kEngineClassCount; ++engine) {
      // The capacity key normalizes a counter that sums concurrent work across engine instances.
      const auto span = static_cast<double>(capacity[engine]);

      double enginePercent = 0.0;
      if (device.driver == Driver::Xe) {
        const std::uint64_t ticksDelta = monotonicDelta(gpuTicks[engine], previousTicks[engine]);
        if (ticksDelta == 0) {
          continue;
        }
        enginePercent = 100.0 * static_cast<double>(busyDelta[engine]) / (static_cast<double>(ticksDelta) * span);
      } else {
        const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previousAt).count();
        if (elapsedNs <= 0) {
          continue;
        }
        enginePercent = 100.0 * static_cast<double>(busyDelta[engine]) / (static_cast<double>(elapsedNs) * span);
      }

      // Render and compute contend for the same execution units, so overall load is the busier of
      // the two, not their sum.
      percent = std::max(percent, enginePercent);
    }

    return UsageReading{.percent = std::clamp(percent, 0.0, 100.0), .source = usageSource(device)};
  }

} // namespace noctalia::system::intel_gpu
