#include "dbus/network/network_glyphs.h"

#include "dbus/network/network_types.h"

namespace network_glyphs {

  const char* glyphForState(const NetworkState& state) noexcept {
    if (state.kind == NetworkConnectivity::Wired) {
      return state.connected ? "ethernet" : "ethernet-off";
    }
    return wifiGlyphForState(state);
  }

  const char* vpnGlyph() noexcept { return "shield-check"; }

  const char* wifiGlyphForState(const NetworkState& state) noexcept {
    if (!state.wirelessEnabled) {
      return "wifi-off";
    }
    if (state.kind == NetworkConnectivity::Unknown) {
      return "wifi-question";
    }
    if (state.kind == NetworkConnectivity::Wireless && state.connected) {
      return wifiGlyphForSignal(state.signalStrength);
    }
    return "wifi-exclamation";
  }

  int wifiSignalBand(std::uint8_t signal) noexcept {
    if (signal >= 80) {
      return 4;
    }
    if (signal >= 60) {
      return 3;
    }
    if (signal >= 35) {
      return 2;
    }
    if (signal >= 15) {
      return 1;
    }
    return 0;
  }

  const char* wifiGlyphForSignal(std::uint8_t signal) noexcept {
    switch (wifiSignalBand(signal)) {
    case 4:
      return "wifi";
    case 3:
      return "wifi-3";
    case 2:
      return "wifi-2";
    case 1:
      return "wifi-1";
    default:
      return "wifi-0";
    }
  }

} // namespace network_glyphs
