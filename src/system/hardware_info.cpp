#include "system/hardware_info.h"

#include "compositors/compositor_detect.h"
#include "i18n/i18n.h"
#include "system/format_units.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <unordered_map>
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

  // /proc/mounts escapes space, tab, newline and backslash as octal \NNN sequences.
  std::string unescapeMountField(const std::string& field) {
    std::string out;
    out.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
      if (field[i] == '\\'
          && i + 3 < field.size()
          && std::isdigit(static_cast<unsigned char>(field[i + 1])) != 0
          && std::isdigit(static_cast<unsigned char>(field[i + 2])) != 0
          && std::isdigit(static_cast<unsigned char>(field[i + 3])) != 0) {
        const int value = (field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + (field[i + 3] - '0');
        out.push_back(static_cast<char>(value));
        i += 3;
      } else {
        out.push_back(field[i]);
      }
    }
    return out;
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
  std::ifstream file{"/proc/mounts"};
  if (!file.is_open()) {
    return {};
  }

  // Collapse btrfs subvolumes and bind mounts (same backing device, multiple mount points) to a single
  // entry, preferring the shortest mount point so the root of the device wins.
  std::unordered_map<std::string, std::string> byDevice;
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream iss{line};
    std::string device;
    std::string mount;
    std::string fstype;
    if (!(iss >> device >> mount >> fstype)) {
      continue;
    }
    if (!device.starts_with("/dev/") || device.starts_with("/dev/loop") || fstype == "squashfs") {
      continue;
    }
    mount = unescapeMountField(mount);
    if (mount == "/boot" || mount.starts_with("/boot/")) {
      continue;
    }
    const auto it = byDevice.find(device);
    if (it == byDevice.end() || mount.size() < it->second.size()) {
      byDevice[device] = mount;
    }
  }

  std::vector<std::string> mounts;
  mounts.reserve(byDevice.size());
  for (auto& [device, mount] : byDevice) {
    mounts.push_back(mount);
  }
  std::ranges::sort(mounts);
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
  return std::format("{} ({:.0f}%)", FormatUnits::formatDecimalBytesUsage(used, total), percent);
}

std::string compositorLabel() { return detectCompositor(); }
