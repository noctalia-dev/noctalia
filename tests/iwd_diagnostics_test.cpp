#include "dbus/network/network_display.h"
#include "dbus/network/network_types.h"
#include "tests/test_check.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace {

  std::uint8_t signalToPercent(std::int16_t dBm) {
    if (dBm <= -100) {
      return 0;
    }
    if (dBm >= -50) {
      return 100;
    }
    return static_cast<std::uint8_t>(2 * (dBm + 100));
  }

  // Mirrors IwdService::refresh GetDiagnostics mapping. Baseline iwd never
  // wrote frequencyMhz; candidate applies Frequency and fallback RSSI.
  void applyStationDiagnostics(
      NetworkState& next, std::optional<std::uint32_t> frequencyMhz, std::optional<std::int16_t> rssiDbm
  ) {
#ifdef IWD_DIAGNOSTICS_BASELINE
    (void)frequencyMhz;
    (void)rssiDbm;
    (void)next;
#else
    if (frequencyMhz.has_value() && *frequencyMhz > 0) {
      next.frequencyMhz = *frequencyMhz;
    }
    if (next.signalStrength == 0 && rssiDbm.has_value()) {
      next.signalStrength = signalToPercent(*rssiDbm);
    }
#endif
  }

} // namespace

int main() {
  NetworkState connected;
  connected.kind = NetworkConnectivity::Wireless;
  connected.connected = true;
  applyStationDiagnostics(connected, 2437, std::nullopt);
  TEST_CHECK(connected.frequencyMhz == 2437);
  TEST_CHECK(network_display::wifiFrequencyBandLabel(connected.frequencyMhz) != nullptr);
  TEST_CHECK(std::string_view(network_display::wifiFrequencyBandLabel(connected.frequencyMhz)) == "2.4 GHz");

  NetworkState five;
  applyStationDiagnostics(five, 5180, std::nullopt);
  TEST_CHECK(std::string_view(network_display::wifiFrequencyBandLabel(five.frequencyMhz)) == "5 GHz");

  NetworkState six;
  applyStationDiagnostics(six, 5955, std::nullopt);
  TEST_CHECK(std::string_view(network_display::wifiFrequencyBandLabel(six.frequencyMhz)) == "6 GHz");

  NetworkState ignoredZero;
  applyStationDiagnostics(ignoredZero, 0, std::nullopt);
  TEST_CHECK(ignoredZero.frequencyMhz == 0);
  TEST_CHECK(network_display::wifiFrequencyBandLabel(ignoredZero.frequencyMhz) == nullptr);

  NetworkState rssiOnly;
  applyStationDiagnostics(rssiOnly, std::nullopt, static_cast<std::int16_t>(-70));
  TEST_CHECK(rssiOnly.signalStrength == 60);
  TEST_CHECK(rssiOnly.frequencyMhz == 0);

  NetworkState keepOrdered;
  keepOrdered.signalStrength = 80;
  applyStationDiagnostics(keepOrdered, 2437, static_cast<std::int16_t>(-90));
  TEST_CHECK(keepOrdered.signalStrength == 80);
  TEST_CHECK(keepOrdered.frequencyMhz == 2437);

  return 0;
}
