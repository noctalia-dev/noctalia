#include "system/rfkill_helper.h"

#include "core/log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/rfkill.h>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  constexpr Logger kLog("rfkill");

  [[nodiscard]] std::optional<std::uint32_t> rfkillTypeFor(RfkillDeviceType type) {
    switch (type) {
    case RfkillDeviceType::Bluetooth:
      return RFKILL_TYPE_BLUETOOTH;
    case RfkillDeviceType::Wlan:
      return RFKILL_TYPE_WLAN;
    }
    return std::nullopt;
  }

  struct RfkillEntry {
    std::uint32_t index = 0;
    std::uint32_t type = 0;
    bool soft = false;
    bool hard = false;
  };

  [[nodiscard]] bool readSysfsUnsigned(const std::string& path, unsigned& out) {
    FILE* file = fopen(path.c_str(), "r");
    if (file == nullptr) {
      return false;
    }
    unsigned value = 0;
    const int scanned = fscanf(file, "%u", &value);
    fclose(file);
    if (scanned != 1) {
      return false;
    }
    out = value;
    return true;
  }

  [[nodiscard]] std::vector<RfkillEntry> listRfkillEntries() {
    std::vector<RfkillEntry> entries;
    DIR* dir = opendir("/sys/class/rfkill");
    if (dir == nullptr) {
      return entries;
    }

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
      const std::string name = ent->d_name;
      if (name.rfind("rfkill", 0) != 0) {
        continue;
      }

      const std::string base = "/sys/class/rfkill/" + name + "/";
      RfkillEntry entry{};
      unsigned value = 0;
      if (!readSysfsUnsigned(base + "index", value)) {
        continue;
      }
      entry.index = value;
      if (!readSysfsUnsigned(base + "type", value)) {
        continue;
      }
      entry.type = value;
      if (readSysfsUnsigned(base + "soft", value)) {
        entry.soft = value != 0;
      }
      if (readSysfsUnsigned(base + "hard", value)) {
        entry.hard = value != 0;
      }
      entries.push_back(entry);
    }
    closedir(dir);
    return entries;
  }

  [[nodiscard]] std::optional<RfkillEntry> findEntry(RfkillDeviceType type) {
    const std::optional<std::uint32_t> wantedType = rfkillTypeFor(type);
    if (!wantedType.has_value()) {
      return std::nullopt;
    }
    for (const RfkillEntry& entry : listRfkillEntries()) {
      if (entry.type == *wantedType) {
        return entry;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::uint32_t> rfkillIndexForNetInterface(std::string_view ifname) {
    if (ifname.empty()) {
      return std::nullopt;
    }
    const std::string phyPath = "/sys/class/net/" + std::string(ifname) + "/phy80211/";
    DIR* dir = opendir(phyPath.c_str());
    if (dir == nullptr) {
      return std::nullopt;
    }

    std::optional<std::uint32_t> index;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
      const std::string name = ent->d_name;
      if (name.rfind("rfkill", 0) != 0) {
        continue;
      }
      unsigned value = 0;
      if (!readSysfsUnsigned(phyPath + name + "/index", value)) {
        continue;
      }
      index = value;
      break;
    }
    closedir(dir);
    return index;
  }

  [[nodiscard]] RfkillSwitchResult setRfkillSoftBlockedByIndex(std::uint32_t index, bool softBlocked) {
    const int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
      return {
          .success = false,
          .hardBlocked = false,
          .detail = std::string("cannot open /dev/rfkill: ") + std::strerror(errno)
      };
    }

    rfkill_event ev{};
    ev.idx = index;
    ev.type = 0;
    ev.op = RFKILL_OP_CHANGE;
    ev.soft = softBlocked ? 1 : 0;
    ev.hard = 0;

    ssize_t written = 0;
    do {
      written = write(fd, &ev, sizeof(ev));
    } while (written < 0 && errno == EINTR);
    const int writeErrno = errno;
    close(fd);

    if (written != static_cast<ssize_t>(sizeof(ev))) {
      return {
          .success = false,
          .hardBlocked = false,
          .detail = written < 0 ? std::string(std::strerror(writeErrno)) : "short write to /dev/rfkill"
      };
    }
    return {.success = true, .detail = {}};
  }

} // namespace

RfkillSwitchResult setRfkillSoftBlocked(RfkillDeviceType type, bool softBlocked) {
  const std::optional<RfkillEntry> entry = findEntry(type);
  if (!entry.has_value()) {
    kLog.debug("setRfkillSoftBlocked: no rfkill entry for type {}", static_cast<unsigned>(type));
    return {.success = false, .detail = "no rfkill switch found"};
  }
  if (entry->hard) {
    return {.success = false, .hardBlocked = true, .detail = "rfkill hard block is active"};
  }
  if (entry->soft == softBlocked) {
    return {.success = true, .detail = {}};
  }
  RfkillSwitchResult result = setRfkillSoftBlockedByIndex(entry->index, softBlocked);
  if (!result.success) {
    kLog.warn("setRfkillSoftBlocked: index {} failed: {}", entry->index, result.detail);
  }
  return result;
}

RfkillSwitchResult setRfkillSoftBlockedForNetInterface(std::string_view ifname, bool softBlocked) {
  const std::optional<std::uint32_t> index = rfkillIndexForNetInterface(ifname);
  if (!index.has_value()) {
    return {.success = false, .detail = "no rfkill switch for interface"};
  }

  const std::optional<std::uint32_t> wantedType = rfkillTypeFor(RfkillDeviceType::Wlan);
  for (const RfkillEntry& entry : listRfkillEntries()) {
    if (entry.index == *index) {
      if (wantedType.has_value() && entry.type != *wantedType) {
        return {.success = false, .detail = "rfkill switch is not WLAN"};
      }
      if (entry.hard) {
        return {.success = false, .hardBlocked = true, .detail = "rfkill hard block is active"};
      }
      if (entry.soft == softBlocked) {
        return {.success = true, .detail = {}};
      }
      RfkillSwitchResult result = setRfkillSoftBlockedByIndex(entry.index, softBlocked);
      if (!result.success) {
        kLog.warn("setRfkillSoftBlockedForNetInterface: index {} failed: {}", entry.index, result.detail);
      }
      return result;
    }
  }

  return setRfkillSoftBlockedByIndex(*index, softBlocked);
}

bool isRfkillSoftBlocked(RfkillDeviceType type) {
  const std::optional<RfkillEntry> entry = findEntry(type);
  return entry.has_value() && entry->soft;
}

bool isRfkillHardBlocked(RfkillDeviceType type) {
  const std::optional<RfkillEntry> entry = findEntry(type);
  return entry.has_value() && entry->hard;
}
