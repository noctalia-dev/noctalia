#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// What the user types to join a Wi-Fi network that does not broadcast its SSID.
// Free of sdbus types for the same reason as enterprise_credentials.h: the
// mapping to what NetworkManager stores is the part worth testing without a bus.
namespace network_hidden {

  // IEEE 802.11 caps the SSID element at 32 octets.
  inline constexpr std::size_t kMaxSsidBytes = 32;

  enum class Validation : std::uint8_t {
    Ok,
    MissingSsid,
    SsidTooLong,
  };

  [[nodiscard]] inline Validation validateSsid(std::string_view ssid) noexcept {
    if (ssid.empty()) {
      return Validation::MissingSsid;
    }
    if (ssid.size() > kMaxSsidBytes) {
      return Validation::SsidTooLong;
    }
    return Validation::Ok;
  }

  // NM's 802-11-wireless.ssid is a byte array, not a string.
  [[nodiscard]] inline std::vector<std::uint8_t> ssidBytes(std::string_view ssid) { return {ssid.begin(), ssid.end()}; }

} // namespace network_hidden
