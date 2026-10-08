#include "dbus/network/iwd_diagnostics.h"
#include "dbus/network/network_display.h"
#include "dbus/network/network_types.h"
#include "tests/test_check.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

int main() {
  NetworkState connected;
  connected.kind = NetworkConnectivity::Wireless;
  connected.connected = true;
  iwd_diagnostics::applyStationDiagnostics(
      connected, {{"Frequency", sdbus::Variant{std::uint32_t{2437}}}, {"RSSI", sdbus::Variant{std::int16_t{-70}}}}
  );
  TEST_CHECK(connected.frequencyMhz == 2437);
  TEST_CHECK(connected.signalStrength == 60);
  TEST_CHECK(network_display::wifiFrequencyBandLabel(connected.frequencyMhz) != nullptr);
  TEST_CHECK(std::string_view(network_display::wifiFrequencyBandLabel(connected.frequencyMhz)) == "2.4 GHz");

  NetworkState five;
  iwd_diagnostics::applyStationDiagnostics(five, {{"Frequency", sdbus::Variant{std::uint16_t{5180}}}});
  TEST_CHECK(five.frequencyMhz == 5180);
  TEST_CHECK(std::string_view(network_display::wifiFrequencyBandLabel(five.frequencyMhz)) == "5 GHz");

  NetworkState six;
  iwd_diagnostics::applyStationDiagnostics(six, {{"Frequency", sdbus::Variant{std::int32_t{5955}}}});
  TEST_CHECK(six.frequencyMhz == 5955);
  TEST_CHECK(std::string_view(network_display::wifiFrequencyBandLabel(six.frequencyMhz)) == "6 GHz");

  const std::array invalidFrequencies{
      sdbus::Variant{std::uint32_t{0}}, sdbus::Variant{std::uint16_t{0}},    sdbus::Variant{std::int32_t{0}},
      sdbus::Variant{std::int32_t{-1}}, sdbus::Variant{std::string{"2437"}},
  };
  for (const auto& frequency : invalidFrequencies) {
    NetworkState state;
    iwd_diagnostics::applyStationDiagnostics(
        state, {{"Frequency", frequency}, {"RSSI", sdbus::Variant{std::int16_t{-70}}}}
    );
    TEST_CHECK(state.frequencyMhz == 0);
    TEST_CHECK(network_display::wifiFrequencyBandLabel(state.frequencyMhz) == nullptr);
    TEST_CHECK(state.signalStrength == 60);
  }

  NetworkState missingFields;
  missingFields.frequencyMhz = 5180;
  iwd_diagnostics::applyStationDiagnostics(missingFields, {});
  TEST_CHECK(missingFields.frequencyMhz == 5180);
  TEST_CHECK(missingFields.signalStrength == 0);

  NetworkState invalidFields;
  invalidFields.frequencyMhz = 5180;
  iwd_diagnostics::applyStationDiagnostics(
      invalidFields, {{"Frequency", sdbus::Variant{std::string{"2437"}}}, {"RSSI", sdbus::Variant{std::string{"-70"}}}}
  );
  TEST_CHECK(invalidFields.frequencyMhz == 5180);
  TEST_CHECK(invalidFields.signalStrength == 0);

  NetworkState unsignedRssi;
  iwd_diagnostics::applyStationDiagnostics(
      unsignedRssi, {{"Frequency", sdbus::Variant{std::uint32_t{2437}}}, {"RSSI", sdbus::Variant{std::uint16_t{70}}}}
  );
  TEST_CHECK(unsignedRssi.frequencyMhz == 2437);
  TEST_CHECK(unsignedRssi.signalStrength == 0);

  const std::array<std::pair<std::int16_t, std::uint8_t>, 7> signalCases{
      {{-110, 0}, {-100, 0}, {-99, 2}, {-70, 60}, {-51, 98}, {-50, 100}, {-40, 100}}
  };
  for (const auto& [rssi, expected] : signalCases) {
    NetworkState state;
    iwd_diagnostics::applyStationDiagnostics(state, {{"RSSI", sdbus::Variant{rssi}}});
    TEST_CHECK(state.signalStrength == expected);
    TEST_CHECK(state.frequencyMhz == 0);
  }

  NetworkState wideRssi;
  iwd_diagnostics::applyStationDiagnostics(wideRssi, {{"RSSI", sdbus::Variant{std::int32_t{-70}}}});
  TEST_CHECK(wideRssi.signalStrength == 60);

  NetworkState keepOrdered;
  keepOrdered.signalStrength = 80;
  iwd_diagnostics::applyStationDiagnostics(
      keepOrdered, {{"Frequency", sdbus::Variant{std::uint32_t{2437}}}, {"RSSI", sdbus::Variant{std::int16_t{-90}}}}
  );
  TEST_CHECK(keepOrdered.signalStrength == 80);
  TEST_CHECK(keepOrdered.frequencyMhz == 2437);

  return 0;
}
