#include "dbus/network/network_display.h"
#include "dbus/network/network_types.h"
#include "tests/test_check.h"

#include <cstring>

int main() {
  // Cellular bands are the wifi bands shifted onto cell-signal-1..5: tabler has
  // no zero-bar cellular glyph, so the weakest band still draws one bar.
  TEST_CHECK(std::strcmp(network_display::cellularGlyphForSignal(0), "cell-signal-1") == 0);
  TEST_CHECK(std::strcmp(network_display::cellularGlyphForSignal(20), "cell-signal-2") == 0);
  TEST_CHECK(std::strcmp(network_display::cellularGlyphForSignal(40), "cell-signal-3") == 0);
  TEST_CHECK(std::strcmp(network_display::cellularGlyphForSignal(70), "cell-signal-4") == 0);
  TEST_CHECK(std::strcmp(network_display::cellularGlyphForSignal(79), "cell-signal-4") == 0);
  TEST_CHECK(std::strcmp(network_display::cellularGlyphForSignal(80), "cell-signal-5") == 0);
  TEST_CHECK(std::strcmp(network_display::cellularGlyphForSignal(100), "cell-signal-5") == 0);

  // Cellular kind without a ModemManager signal to band on.
  NetworkState cellular;
  cellular.kind = NetworkConnectivity::Cellular;
  cellular.connected = true;
  TEST_CHECK(std::strcmp(network_display::glyphForState(cellular), "cell-signal-1") == 0);
  cellular.connected = false;
  TEST_CHECK(std::strcmp(network_display::glyphForState(cellular), "cell-signal-off") == 0);

  // A Wi-Fi device running a hotspot is in use, not failing to connect.
  NetworkState hotspot;
  hotspot.kind = NetworkConnectivity::Cellular;
  hotspot.connected = true;
  hotspot.wirelessEnabled = true;
  hotspot.hotspotActive = true;
  TEST_CHECK(std::strcmp(network_display::wifiGlyphForState(hotspot), "access-point") == 0);
  hotspot.hotspotActive = false;
  TEST_CHECK(std::strcmp(network_display::wifiGlyphForState(hotspot), "wifi-exclamation") == 0);

  return 0;
}
