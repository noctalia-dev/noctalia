#pragma once

#include <cstdint>
#include <string>
#include <string_view>

enum class RfkillDeviceType : std::uint8_t {
  Bluetooth,
  Wlan,
};

struct [[nodiscard]] RfkillSwitchResult {
  bool success = false;
  // Diagnostic only: a hard block was observed while writing. Vendor platform switches report stale
  // hard blocks, so callers must not treat this as a reason to skip the radio operation.
  bool hardBlocked = false;
  std::string detail;
};

/// Clears or sets the rfkill soft block for every switch of the given type.
RfkillSwitchResult setRfkillSoftBlocked(RfkillDeviceType type, bool softBlocked);

/// Clears or sets rfkill soft block for the WLAN switch tied to a network interface (e.g. wlan0).
RfkillSwitchResult setRfkillSoftBlockedForNetInterface(std::string_view ifname, bool softBlocked);

/// True when any switch of this type is soft-blocked.
bool isRfkillSoftBlocked(RfkillDeviceType type);
