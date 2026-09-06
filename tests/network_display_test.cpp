#include "dbus/network/network_display.h"
#include "dbus/network/network_types.h"

#include <cstring>
#include <print>

namespace {

  bool expectGlyph(const char* actual, const char* expected, const char* message) {
    if (std::strcmp(actual, expected) != 0) {
      std::println(stderr, "network_display_test: {}: expected '{}', got '{}'", message, expected, actual);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  bool ok = true;

  ok = expectGlyph(network_display::cellularGlyphForSignal(0), "cell-signal-1", "no cellular signal") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(14), "cell-signal-1", "band 0 upper edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(15), "cell-signal-2", "band 1 lower edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(34), "cell-signal-2", "band 1 upper edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(35), "cell-signal-3", "band 2 lower edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(59), "cell-signal-3", "band 2 upper edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(60), "cell-signal-4", "band 3 lower edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(79), "cell-signal-4", "band 3 upper edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(80), "cell-signal-5", "band 4 lower edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(100), "cell-signal-5", "full cellular signal") && ok;
  ok = expectGlyph(network_display::cellularOffGlyph(), "cell-signal-off", "cellular off glyph") && ok;

  NetworkState cellular;
  cellular.kind = NetworkConnectivity::Cellular;
  cellular.connected = true;
  ok = expectGlyph(network_display::glyphForState(cellular), "cell-signal-1", "connected cellular fallback") && ok;
  cellular.connected = false;
  ok = expectGlyph(network_display::glyphForState(cellular), "cell-signal-off", "disconnected cellular fallback") && ok;

  NetworkState wired;
  wired.kind = NetworkConnectivity::Wired;
  wired.connected = true;
  ok = expectGlyph(network_display::glyphForState(wired), "ethernet", "wired unaffected") && ok;

  NetworkState wireless;
  wireless.kind = NetworkConnectivity::Wireless;
  wireless.connected = true;
  wireless.wirelessEnabled = true;
  wireless.signalStrength = 90;
  ok = expectGlyph(network_display::glyphForState(wireless), "wifi", "wireless unaffected") && ok;

  return ok ? 0 : 1;
}
