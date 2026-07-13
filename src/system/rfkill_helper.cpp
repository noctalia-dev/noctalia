#include "system/rfkill_helper.h"

#include "core/log.h"

#include <algorithm>
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

  [[nodiscard]] std::optional<std::string_view> rfkillTypeStringFor(RfkillDeviceType type) {
    switch (type) {
    case RfkillDeviceType::Bluetooth:
      return "bluetooth";
    case RfkillDeviceType::Wlan:
      return "wlan";
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::uint8_t> rfkillTypeIdFor(RfkillDeviceType type) {
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
    std::string type;
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

  [[nodiscard]] std::optional<std::string> readSysfsString(const std::string& path) {
    FILE* file = fopen(path.c_str(), "r");
    if (file == nullptr) {
      return std::nullopt;
    }
    char buf[32]{};
    const bool scanned = fscanf(file, "%31s", buf) == 1;
    fclose(file);
    if (!scanned) {
      return std::nullopt;
    }
    return buf;
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
      if (!name.starts_with("rfkill")) {
        continue;
      }

      const std::string base = "/sys/class/rfkill/" + name + "/";
      RfkillEntry entry{};
      unsigned value = 0;
      if (!readSysfsUnsigned(base + "index", value)) {
        continue;
      }
      entry.index = value;
      auto typeStr = readSysfsString(base + "type");
      if (!typeStr.has_value()) {
        continue;
      }
      entry.type = std::move(*typeStr);
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

  [[nodiscard]] std::vector<RfkillEntry> findEntries(RfkillDeviceType type) {
    const std::optional<std::string_view> wantedType = rfkillTypeStringFor(type);
    std::vector<RfkillEntry> matches;
    if (!wantedType.has_value()) {
      return matches;
    }
    for (const RfkillEntry& entry : listRfkillEntries()) {
      if (entry.type == *wantedType) {
        matches.push_back(entry);
      }
    }
    return matches;
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
      if (!name.starts_with("rfkill")) {
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

  [[nodiscard]] RfkillSwitchResult writeRfkillEvent(const rfkill_event& ev) {
    const int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
      return {
          .success = false,
          .hardBlocked = false,
          .detail = std::string("cannot open /dev/rfkill: ") + std::strerror(errno)
      };
    }

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

  [[nodiscard]] RfkillSwitchResult setRfkillSoftBlockedByIndex(std::uint32_t index, bool softBlocked) {
    rfkill_event ev{};
    ev.idx = index;
    ev.type = 0;
    ev.op = RFKILL_OP_CHANGE;
    ev.soft = softBlocked ? 1 : 0;
    ev.hard = 0;
    return writeRfkillEvent(ev);
  }

  // RFKILL_OP_CHANGE_ALL applies the soft block to every switch of the type, and to switches of that
  // type that appear later. Radios commonly sit behind several switches (driver plus vendor platform
  // module), and all of them must be unblocked for the radio to come up.
  [[nodiscard]] RfkillSwitchResult setRfkillSoftBlockedByType(std::uint8_t typeId, bool softBlocked) {
    rfkill_event ev{};
    ev.idx = 0;
    ev.type = typeId;
    ev.op = RFKILL_OP_CHANGE_ALL;
    ev.soft = softBlocked ? 1 : 0;
    ev.hard = 0;
    return writeRfkillEvent(ev);
  }

} // namespace

RfkillSwitchResult setRfkillSoftBlocked(RfkillDeviceType type, bool softBlocked) {
  const std::optional<std::uint8_t> typeId = rfkillTypeIdFor(type);
  const std::vector<RfkillEntry> entries = findEntries(type);
  if (!typeId.has_value() || entries.empty()) {
    kLog.debug("setRfkillSoftBlocked: no rfkill entry for type {}", static_cast<unsigned>(type));
    return {.success = false, .detail = "no rfkill switch found"};
  }
  if (std::ranges::any_of(entries, &RfkillEntry::hard)) {
    return {.success = false, .hardBlocked = true, .detail = "rfkill hard block is active"};
  }
  if (std::ranges::all_of(entries, [softBlocked](const RfkillEntry& entry) { return entry.soft == softBlocked; })) {
    return {.success = true, .detail = {}};
  }

  RfkillSwitchResult result = setRfkillSoftBlockedByType(*typeId, softBlocked);
  if (!result.success) {
    kLog.warn("setRfkillSoftBlocked: type {} failed: {}", static_cast<unsigned>(type), result.detail);
  }
  return result;
}

RfkillSwitchResult setRfkillSoftBlockedForNetInterface(std::string_view ifname, bool softBlocked) {
  const std::optional<std::uint32_t> index = rfkillIndexForNetInterface(ifname);
  if (!index.has_value()) {
    return {.success = false, .detail = "no rfkill switch for interface"};
  }

  const std::optional<std::string_view> wantedType = rfkillTypeStringFor(RfkillDeviceType::Wlan);
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

// A radio is blocked when any of its switches blocks it.
bool isRfkillSoftBlocked(RfkillDeviceType type) { return std::ranges::any_of(findEntries(type), &RfkillEntry::soft); }

bool isRfkillHardBlocked(RfkillDeviceType type) { return std::ranges::any_of(findEntries(type), &RfkillEntry::hard); }
