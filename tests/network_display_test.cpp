#include "dbus/network/network_display.h"
#include "dbus/network/network_types.h"
#include "tests/test_check.h"

#include <limits>
#include <string_view>

int main() {
  NetworkState state{
      .kind = NetworkConnectivity::Cellular,
      .connected = false,
      .resolving = false,
      .wirelessEnabled = true,
      .cellularEnabled = false,
  };

  TEST_CHECK(std::string_view(network_display::glyphForState(state)) == "antenna-bars-off");

  state.cellularEnabled = true;
  TEST_CHECK(std::string_view(network_display::glyphForState(state)) == "antenna");

  state.resolving = true;
  TEST_CHECK(std::string_view(network_display::glyphForState(state)) == "antenna-bars-1");

  state.resolving = false;
  state.connected = true;
  TEST_CHECK(std::string_view(network_display::glyphForState(state)) == "antenna-bars-1");

  state.cellularSignalStrength = 15;
  TEST_CHECK(std::string_view(network_display::glyphForState(state)) == "antenna-bars-2");
  state.cellularSignalStrength = 35;
  TEST_CHECK(std::string_view(network_display::glyphForState(state)) == "antenna-bars-3");
  state.cellularSignalStrength = 60;
  TEST_CHECK(std::string_view(network_display::glyphForState(state)) == "antenna-bars-4");
  state.cellularSignalStrength = 80;
  TEST_CHECK(std::string_view(network_display::glyphForState(state)) == "antenna-bars-5");
  TEST_CHECK(std::string_view(network_display::cellularGlyphForSignal(0)) == "antenna-bars-1");
  TEST_CHECK(std::string_view(network_display::cellularGlyphForSignal(35)) == "antenna-bars-3");
  TEST_CHECK(std::string_view(network_display::cellularGlyphForSignal(80)) == "antenna-bars-5");

  CellularConnectionInfo cellular{
      .path = "/profile/1",
      .name = "cellular",
      .devicePath = "/device/1",
      .active = true,
  };
  state.cellularDevicePath = "/device/1";
  TEST_CHECK(network_display::shouldShowCellularSignal(cellular, state));
  cellular.active = false;
  TEST_CHECK(!network_display::shouldShowCellularSignal(cellular, state));
  cellular.active = true;
  cellular.devicePath = "/device/2";
  TEST_CHECK(!network_display::shouldShowCellularSignal(cellular, state));

  TEST_CHECK(network_display::cellularSignalPercentFromRsrp(-111.0) == 0);
  TEST_CHECK(network_display::cellularSignalPercentFromRsrp(-110.0) == 0);
  TEST_CHECK(network_display::cellularSignalPercentFromRsrp(-109.6) == 0);
  TEST_CHECK(network_display::cellularSignalPercentFromRsrp(-109.5) == 1);
  TEST_CHECK(network_display::cellularSignalPercentFromRsrp(-85.0) == 50);
  TEST_CHECK(network_display::cellularSignalPercentFromRsrp(-60.0) == 100);
  TEST_CHECK(network_display::cellularSignalPercentFromRsrp(-59.0) == 100);
  TEST_CHECK(network_display::cellularSignalPercentFromRsrp(std::numeric_limits<double>::quiet_NaN()) == 0);

  TEST_CHECK(network_display::cellularAccessTechnologyLabel(0) == nullptr);
  TEST_CHECK(std::string_view(network_display::cellularAccessTechnologyLabel(1U << 1U)) == "GSM");
  TEST_CHECK(std::string_view(network_display::cellularAccessTechnologyLabel(1U << 5U)) == "UMTS");
  TEST_CHECK(std::string_view(network_display::cellularAccessTechnologyLabel(1U << 14U)) == "LTE");
  TEST_CHECK(std::string_view(network_display::cellularAccessTechnologyLabel(1U << 15U)) == "5G NR");
  TEST_CHECK(std::string_view(network_display::cellularAccessTechnologyLabel((1U << 14U) | (1U << 15U))) == "5G NR");

  return 0;
}