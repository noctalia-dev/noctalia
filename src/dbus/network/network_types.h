#pragma once

#include <cstdint>
#include <string>

struct AccessPointInfo {
  std::string path;       // Backend access point object path.
  std::string devicePath; // Backend device object path this AP belongs to.
  std::string ssid;
  std::uint8_t strength = 0; // 0..100
  bool secured = false;
  bool supportsSae = false;
  bool active = false;

  bool operator==(const AccessPointInfo&) const = default;
};

struct VpnConnectionInfo {
  std::string path; // Backend settings connection object path.
  std::string name;
  bool active = false;

  bool operator==(const VpnConnectionInfo&) const = default;
};

enum class NetworkConnectivity {
  Unknown = 0,
  None = 1,
  Wired = 2,
  Wireless = 3,
};

struct NetworkState {
  NetworkConnectivity kind = NetworkConnectivity::Unknown;
  bool connected = false;
  bool resolving = false; // active connection is activating, not yet connected
  bool wirelessEnabled = false;
  bool scanning = false;
  bool vpnActive = false;          // a VPN connection is active or activating
  bool vpnConnected = false;       // a VPN tunnel is fully activated (routes applied)
  std::string ssid;                // Wi-Fi only
  std::string ipv4;                // dotted-quad of first address; empty if none
  std::string interfaceName;       // e.g. "wlan0", "eth0"
  std::uint8_t signalStrength = 0; // 0..100, Wi-Fi only
  // Operating frequency of the associated BSS. Wi-Fi only; 0 when the backend
  // does not report one (iwd).
  std::uint32_t frequencyMhz = 0;

  bool operator==(const NetworkState&) const = default;
};

enum class NetworkChangeOrigin : std::uint8_t {
  External,
  Noctalia,
};
