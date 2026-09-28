#include "dbus/network/wpa_supplicant_utils.h"
#include "tests/test_check.h"

#include <cstdint>
#include <string>
#include <vector>

int main() {
  const std::vector<std::uint8_t> expected{0x22, 0x43, 0x61, 0x66, 0xc3, 0xa9, 0x00, 0xff, 0x22};
  const std::string ssid{reinterpret_cast<const char*>(expected.data()), expected.size()};

  const sdbus::Variant variant = wpa_supplicant::ssidVariant(ssid);

  TEST_CHECK(variant.containsValueOfType<std::vector<std::uint8_t>>());
  TEST_CHECK(!variant.containsValueOfType<std::string>());
  TEST_CHECK(variant.get<std::vector<std::uint8_t>>() == expected);
}
