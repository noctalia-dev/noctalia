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

  // Cellular bands are the wifi bands shifted onto cell-signal-1..5: tabler has
  // no zero-bar cellular glyph, so the weakest band still draws one bar.
  ok = expectGlyph(network_display::cellularGlyphForSignal(0), "cell-signal-1", "no signal") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(20), "cell-signal-2", "weak signal") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(40), "cell-signal-3", "medium signal") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(70), "cell-signal-4", "strong signal") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(79), "cell-signal-4", "top band lower edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(80), "cell-signal-5", "top band") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(100), "cell-signal-5", "full signal") && ok;

  // Cellular kind without a ModemManager signal to band on.
  NetworkState cellular;
  cellular.kind = NetworkConnectivity::Cellular;
  cellular.connected = true;
  ok = expectGlyph(network_display::glyphForState(cellular), "cell-signal-1", "connected cellular fallback") && ok;
  cellular.connected = false;
  ok = expectGlyph(network_display::glyphForState(cellular), "cell-signal-off", "disconnected cellular fallback") && ok;

  return ok ? 0 : 1;
}
