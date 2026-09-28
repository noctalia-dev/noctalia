#pragma once

#include <cstdint>
#include <sdbus-c++/Types.h>
#include <string_view>
#include <vector>

namespace wpa_supplicant {

  [[nodiscard]] inline sdbus::Variant ssidVariant(std::string_view ssid) {
    return sdbus::Variant{std::vector<std::uint8_t>{ssid.begin(), ssid.end()}};
  }

} // namespace wpa_supplicant
