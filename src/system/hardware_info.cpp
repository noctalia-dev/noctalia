#include "system/hardware_info.h"

#include "compositors/compositor_detect.h"
#include "core/deferred_call.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "system/disk_mounts.h"
#include "system/format_units.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/statvfs.h>
#include <vector>

namespace {

  std::string readCpuModel() {
    std::ifstream file{"/proc/cpuinfo"};
    if (!file.is_open()) {
      return i18n::tr("system.hardware.unknown-cpu");
    }

    std::string line;
    while (std::getline(file, line)) {
      if (line.starts_with("model name")) {
        const auto colonPos = line.find(':');
        if (colonPos != std::string::npos) {
          return StringUtils::trim(line.substr(colonPos + 1));
        }
      }
    }
    return i18n::tr("system.hardware.unknown-cpu");
  }

  std::string shortVendorPrefix(const std::string& vendorName) {
    if (vendorName.contains("AMD") || vendorName.contains("ATI")) {
      return "AMD";
    }
    if (vendorName.contains("NVIDIA")) {
      return "NVIDIA";
    }
    if (vendorName.contains("Intel")) {
      return "Intel";
    }
    return {};
  }

  std::string formatGpuName(const std::string& vendorName, const std::string& rawName) {
    std::string prefix = shortVendorPrefix(vendorName);
    std::string name = rawName;

    // pci.ids uses brackets inconsistently:
    //   device line:    "Navi 32 [Radeon RX 7700 XT / 7800 XT]"  → bracket = marketing name
    //   subsystem line: "RX 7800 XT [Hellhound / Red Devil]"     → bracket = board partner
    // Heuristic: if text before '[' already looks like a GPU model (contains "RX"/"GTX"/"RTX"/"Arc"/"HD"),
    // keep that and drop the bracket content. Otherwise use the bracket content.
    const auto bracketOpen = rawName.find('[');
    const auto bracketClose = rawName.rfind(']');
    if (bracketOpen != std::string::npos && bracketClose != std::string::npos && bracketClose > bracketOpen) {
      std::string before = StringUtils::trim(rawName.substr(0, bracketOpen));
      std::string inside = rawName.substr(bracketOpen + 1, bracketClose - bracketOpen - 1);

      const bool beforeIsModel = before.contains("RX")
          || before.contains("GTX")
          || before.contains("RTX")
          || before.contains("Arc")
          || before.contains("HD ");
      name = beforeIsModel ? before : inside;
    }

    if (!prefix.empty() && !name.contains(prefix)) {
      return prefix + " " + name;
    }
    return name;
  }

  // libdrm's amdgpu.ids maps device and revision IDs to a marketing name.
  // This is generally more specific than pci.ids subsystem names.
  std::string lookupAmdGpuIds(const std::string& deviceId, const std::string& revisionId) {
    std::ifstream file{"/usr/share/libdrm/amdgpu.ids"};
    if (!file.is_open()) {
      return {};
    }

    std::string line;
    while (std::getline(file, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }

      const auto firstComma = line.find(',');
      const auto secondComma = firstComma == std::string::npos ? std::string::npos : line.find(',', firstComma + 1);
      if (firstComma == std::string::npos || secondComma == std::string::npos) {
        continue;
      }

      const auto fileDevice = StringUtils::trim(line.substr(0, firstComma));
      const auto fileRevision = StringUtils::trim(line.substr(firstComma + 1, secondComma - firstComma - 1));

      if (StringUtils::equalsInsensitive(fileDevice, deviceId)
          && StringUtils::equalsInsensitive(fileRevision, revisionId)) {
        return StringUtils::trim(line.substr(secondComma + 1));
      }
    }

    return {};
  }

  std::string lookupPciIds(
      const std::string& vendorId, const std::string& deviceId, const std::string& subVendorId = {},
      const std::string& subDeviceId = {}
  ) {
    std::ifstream file{"/usr/share/hwdata/pci.ids"};
    if (!file.is_open()) {
      return {};
    }

    std::string line;
    bool inVendor = false;
    bool inDevice = false;
    std::string vendorName;
    std::string deviceName;

    while (std::getline(file, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }

      if (line[0] != '\t') {
        if (inVendor) {
          break;
        }
        if (line.starts_with(vendorId)) {
          const auto nameStart = line.find("  ");
          if (nameStart != std::string::npos) {
            vendorName = StringUtils::trim(line.substr(nameStart));
            inVendor = true;
          }
        }
        continue;
      }

      if (!inVendor) {
        continue;
      }

      // Subsystem line (two tabs)
      if (line.size() >= 2 && line[0] == '\t' && line[1] == '\t') {
        if (inDevice && !subVendorId.empty() && !subDeviceId.empty()) {
          auto stripped = StringUtils::trim(line);
          const std::string subKey = subVendorId + " " + subDeviceId;
          if (stripped.starts_with(subKey)) {
            const auto nameStart = stripped.find("  ");
            if (nameStart != std::string::npos) {
              auto subName = StringUtils::trim(stripped.substr(nameStart));
              if (!subName.empty()) {
                return formatGpuName(vendorName, subName);
              }
            }
          }
        }
        continue;
      }

      // Device line (one tab)
      if (inDevice) {
        break;
      }
      auto stripped = StringUtils::trim(line);
      if (stripped.starts_with(deviceId)) {
        const auto nameStart = stripped.find("  ");
        if (nameStart != std::string::npos) {
          deviceName = StringUtils::trim(stripped.substr(nameStart));
          inDevice = true;
        }
      }
    }

    if (!deviceName.empty()) {
      return formatGpuName(vendorName, deviceName);
    }
    if (!vendorName.empty()) {
      return shortVendorPrefix(vendorName).empty() ? vendorName : shortVendorPrefix(vendorName);
    }
    return {};
  }

  std::string readSysfsLine(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file.is_open()) {
      return {};
    }
    std::string line;
    std::getline(file, line);
    return StringUtils::trim(line);
  }

  std::string detectGpu() {
    namespace fs = std::filesystem;

    const fs::path drmRoot{"/sys/class/drm"};
    if (!fs::exists(drmRoot) || !fs::is_directory(drmRoot)) {
      return i18n::tr("system.hardware.unknown-gpu");
    }

    std::vector<fs::path> cardPaths;
    for (const auto& entry : fs::directory_iterator{drmRoot}) {
      const auto name = entry.path().filename().string();
      if (!name.starts_with("card") || name.contains('-')) {
        continue;
      }
      cardPaths.push_back(entry.path());
    }
    std::ranges::sort(cardPaths);

    std::vector<std::string> gpus;
    for (const auto& cardPath : cardPaths) {
      const auto name = cardPath.filename().string();

      const auto deviceDir = cardPath / "device";
      if (!fs::exists(deviceDir)) {
        continue;
      }

      auto driverLine = readSysfsLine(deviceDir / "uevent");
      // uevent has multiple lines; re-read and search for DRIVER=
      std::string driver;
      {
        std::ifstream uevent{deviceDir / "uevent"};
        std::string line;
        while (std::getline(uevent, line)) {
          if (line.starts_with("DRIVER=")) {
            driver = line.substr(7);
            break;
          }
        }
      }

      if (driver == "simpledrm") {
        continue;
      }

      auto vendorHex = readSysfsLine(deviceDir / "vendor");
      auto deviceHex = readSysfsLine(deviceDir / "device");
      auto subVendorHex = readSysfsLine(deviceDir / "subsystem_vendor");
      auto subDeviceHex = readSysfsLine(deviceDir / "subsystem_device");

      auto stripHexPrefix = [](std::string& s) {
        if (s.starts_with("0x") || s.starts_with("0X")) {
          s = s.substr(2);
        }
      };
      stripHexPrefix(vendorHex);
      stripHexPrefix(deviceHex);
      stripHexPrefix(subVendorHex);
      stripHexPrefix(subDeviceHex);

      if (!vendorHex.empty() && !deviceHex.empty()) {
        if (vendorHex == "1002") {
          auto revisionHex = readSysfsLine(deviceDir / "revision");
          stripHexPrefix(revisionHex);
          auto amdName = lookupAmdGpuIds(deviceHex, revisionHex);
          if (!amdName.empty()) {
            if (!amdName.contains("AMD")) {
              amdName = "AMD " + amdName;
            }
            if (!std::ranges::contains(gpus, amdName)) {
              gpus.push_back(std::move(amdName));
            }
            continue;
          }
        }

        auto pciName = lookupPciIds(vendorHex, deviceHex, subVendorHex, subDeviceHex);
        if (!pciName.empty()) {
          if (!std::ranges::contains(gpus, pciName)) {
            gpus.push_back(std::move(pciName));
          }
          continue;
        }
      }

      if (!driver.empty()) {
        std::string label;
        if (driver == "i915" || driver == "xe") {
          label = "Intel GPU (" + driver + ")";
        } else if (driver == "amdgpu" || driver == "radeon") {
          label = "AMD GPU (" + driver + ")";
        } else if (driver == "nvidia" || driver == "nouveau") {
          label = "NVIDIA GPU (" + driver + ")";
        } else {
          label = driver + " GPU";
        }
        if (!std::ranges::contains(gpus, label)) {
          gpus.push_back(std::move(label));
        }
      }
    }

    if (!gpus.empty()) {
      return StringUtils::join(gpus, " · ");
    }

    return i18n::tr("system.hardware.unknown-gpu");
  }

  std::string readDmiField(const char* path) { return readSysfsLine(path); }

  std::string detectMotherboard() {
    const std::string boardName = readDmiField("/sys/class/dmi/id/board_name");
    const std::string boardVersion = readDmiField("/sys/class/dmi/id/board_version");
    const std::string productName = readDmiField("/sys/class/dmi/id/product_name");

    if (!boardName.empty()) {
      if (!boardVersion.empty()) {
        return boardName + " (" + boardVersion + ")";
      }
      return boardName;
    }
    if (!productName.empty()) {
      return productName;
    }
    return i18n::tr("system.hardware.unknown");
  }

  std::string detectMemoryTotal() {
    std::ifstream file{"/proc/meminfo"};
    if (!file.is_open()) {
      return i18n::tr("system.hardware.unknown");
    }
    std::string line;
    while (std::getline(file, line)) {
      if (!line.starts_with("MemTotal:")) {
        continue;
      }
      const std::size_t valueStart = line.find_first_of("0123456789");
      if (valueStart == std::string::npos) {
        break;
      }
      const std::size_t valueEnd = line.find_first_not_of("0123456789", valueStart);
      const std::string kbText = line.substr(valueStart, valueEnd - valueStart);
      std::uint64_t totalKb = 0;
      try {
        totalKb = static_cast<std::uint64_t>(std::stoull(kbText));
      } catch (...) {
        return i18n::tr("system.hardware.unknown");
      }
      return FormatUnits::formatBinaryBytesAsGib(totalKb * 1024ULL);
    }
    return i18n::tr("system.hardware.unknown");
  }

  std::string detectCompositor() {
    const auto kind = compositors::detect();
    if (kind != compositors::CompositorKind::Unknown) {
      return std::string(compositors::name(kind));
    }
    // Fall back to whatever the desktop env vars say, which is friendlier than "Unknown"
    // for compositors we don't have a backend for (KDE, GNOME, etc.).
    if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP"); desktop != nullptr && desktop[0] != '\0') {
      return desktop;
    }
    if (const char* sessionDesktop = std::getenv("XDG_SESSION_DESKTOP");
        sessionDesktop != nullptr && sessionDesktop[0] != '\0') {
      return sessionDesktop;
    }
    return i18n::tr("system.hardware.unknown");
  }

  std::vector<std::string> compositorVersionCommand(compositors::CompositorKind kind) {
    switch (kind) {
    case compositors::CompositorKind::Niri:
      return {"niri", "--version"};
    case compositors::CompositorKind::Hyprland:
      return {"Hyprland", "--version"};
    case compositors::CompositorKind::Sway:
      return {"sway", "--version"};
    case compositors::CompositorKind::Mango:
      return {"mango", "-v"};
    case compositors::CompositorKind::Dwl:
      return {"dwl", "-v"};
    case compositors::CompositorKind::Labwc:
      return {"labwc", "--version"};
    case compositors::CompositorKind::Kde:
      return {"kwin_wayland", "--version"};
    case compositors::CompositorKind::Umbriel:
      return {"umbriel", "-v"};
    case compositors::CompositorKind::Unknown:
      break;
    }
    return {};
  }

  // First non-blank line, verbatim apart from surrounding whitespace and an ASCII-capitalized first letter
  // (e.g. "Umbriel 0.1.0 (350794892e2b)").
  std::string firstNonBlankLine(std::string_view text) {
    const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty()) {
      const std::size_t end = text.find('\n');
      std::string_view line = text.substr(0, end);
      while (!line.empty() && isSpace(line.front())) {
        line.remove_prefix(1);
      }
      while (!line.empty() && isSpace(line.back())) {
        line.remove_suffix(1);
      }
      if (!line.empty()) {
        std::string result(line);
        if (result.front() >= 'a' && result.front() <= 'z') {
          result.front() = static_cast<char>(result.front() - 'a' + 'A');
        }
        return result;
      }
      if (end == std::string_view::npos) {
        break;
      }
      text.remove_prefix(end + 1);
    }
    return {};
  }

} // namespace

std::string cpuModelName() {
  static std::once_flag flag;
  static std::string cached;
  std::call_once(flag, [&]() { cached = readCpuModel(); });
  return cached;
}

std::string gpuLabel() {
  static std::once_flag flag;
  static std::string cached;
  std::call_once(flag, [&]() { cached = detectGpu(); });
  return cached;
}

std::string motherboardLabel() {
  static std::once_flag flag;
  static std::string cached;
  std::call_once(flag, [&]() { cached = detectMotherboard(); });
  return cached;
}

std::string memoryTotalLabel() {
  static std::once_flag flag;
  static std::string cached;
  std::call_once(flag, [&]() { cached = detectMemoryTotal(); });
  return cached;
}

std::vector<std::string> physicalDiskMountPoints() {
  auto disks = physicalDiskMounts();
  std::vector<std::string> mounts;
  mounts.reserve(disks.size());
  for (auto& disk : disks) {
    mounts.push_back(std::move(disk.path));
  }
  return mounts;
}

std::string diskUsageLabel(const std::string& mountPoint) {
  struct statvfs sv{};
  if (::statvfs(mountPoint.c_str(), &sv) != 0 || sv.f_blocks == 0 || sv.f_frsize == 0) {
    return i18n::tr("system.hardware.unknown");
  }
  const double total = static_cast<double>(sv.f_blocks) * static_cast<double>(sv.f_frsize);
  const double avail = static_cast<double>(sv.f_bavail) * static_cast<double>(sv.f_frsize);
  const double used = std::max(0.0, total - avail);
  const double percent = total > 0.0 ? (used / total) * 100.0 : 0.0;
  return std::format("{} ({:.0F}%)", FormatUnits::formatDecimalBytesUsage(used, total), percent);
}

std::string compositorLabel() { return detectCompositor(); }

std::optional<std::string> compositorVersionLine(std::function<void()> onResolved) {
  enum class State : std::uint8_t { Idle, Pending, Resolved };
  static State state = State::Idle;
  static std::string version;
  static std::function<void()> pendingCallback;

  if (state == State::Resolved) {
    return version;
  }
  pendingCallback = std::move(onResolved);
  if (state == State::Pending) {
    return std::nullopt;
  }

  const auto finish = [](std::string resolved) {
    version = std::move(resolved);
    state = State::Resolved;
    if (auto callback = std::move(pendingCallback); callback) {
      callback();
    }
  };

  const std::vector<std::string> command = compositorVersionCommand(compositors::detect());
  if (command.empty() || !process::commandExists(command.front().c_str())) {
    state = State::Resolved;
    version.clear();
    pendingCallback = nullptr;
    return version;
  }

  state = State::Pending;
  process::RunOptions options;
  options.timeout = std::chrono::seconds(3);
  options.maxOutputBytes = 4096;
  const bool launched = process::runAsync(
      command,
      process::RunCallbacks{
          .onExit =
              [finish](process::RunResult result) {
                // Worker thread: extract here, publish on the main loop. Some compositors print to stderr.
                std::string parsed = result.timedOut ? std::string{} : firstNonBlankLine(result.out);
                if (parsed.empty() && !result.timedOut) {
                  parsed = firstNonBlankLine(result.err);
                }
                DeferredCall::callLater([finish, parsed = std::move(parsed)]() mutable { finish(std::move(parsed)); });
              },
      },
      options
  );
  if (!launched) {
    state = State::Resolved;
    version.clear();
    pendingCallback = nullptr;
    return version;
  }
  return std::nullopt;
}
