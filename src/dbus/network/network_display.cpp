#include "dbus/network/network_display.h"

#include "dbus/network/network_types.h"

#include <cmath>

namespace network_display {

  const char* glyphForState(const NetworkState& state) noexcept {
    if (state.kind == NetworkConnectivity::Wired) {
      return state.connected ? "ethernet" : "ethernet-off";
    }
    if (state.kind == NetworkConnectivity::Cellular) {
      return cellularGlyphForState(state);
    }
    return wifiGlyphForState(state);
  }

  const char* vpnGlyph() noexcept { return "shield-check"; }

  const char* cellularGlyphForState(const NetworkState& state) noexcept {
    if (!state.cellularEnabled) {
      return "antenna-bars-off";
    }
    if (state.kind == NetworkConnectivity::Cellular && state.connected) {
      return cellularGlyphForSignal(state.cellularSignalStrength);
    }
    if (state.resolving) {
      return "antenna-bars-1";
    }
    return "antenna";
  }

  const char* cellularGlyphForSignal(std::uint8_t signal) noexcept {
    switch (wifiSignalBand(signal)) {
    case 4:
      return "antenna-bars-5";
    case 3:
      return "antenna-bars-4";
    case 2:
      return "antenna-bars-3";
    case 1:
      return "antenna-bars-2";
    default:
      return "antenna-bars-1";
    }
  }

  bool shouldShowCellularSignal(const CellularConnectionInfo& connection, const NetworkState& state) noexcept {
    return connection.active && !connection.devicePath.empty() && connection.devicePath == state.cellularDevicePath;
  }

  std::uint8_t cellularSignalPercentFromRsrp(double rsrpDbm) noexcept {
    if (!std::isfinite(rsrpDbm) || rsrpDbm <= -110.0) {
      return 0;
    }
    if (rsrpDbm >= -60.0) {
      return 100;
    }
    return static_cast<std::uint8_t>((rsrpDbm + 110.0) * 2.0);
  }

  const char* cellularAccessTechnologyLabel(std::uint32_t technologies) noexcept {
    if ((technologies & (1U << 15U)) != 0U) {
      return "5G NR";
    }
    if ((technologies & (1U << 14U)) != 0U) {
      return "LTE";
    }
    if ((technologies & (1U << 9U)) != 0U) {
      return "HSPA+";
    }
    if ((technologies & (1U << 8U)) != 0U) {
      return "HSPA";
    }
    if ((technologies & (1U << 5U)) != 0U) {
      return "UMTS";
    }
    if ((technologies & (1U << 4U)) != 0U) {
      return "EDGE";
    }
    if ((technologies & (1U << 3U)) != 0U) {
      return "GPRS";
    }
    if ((technologies & (1U << 1U)) != 0U) {
      return "GSM";
    }
    return nullptr;
  }

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

  const char* wifiFrequencyBandLabel(std::uint32_t frequencyMhz) noexcept {
    // Broad envelopes around each band rather than exact channel centers — the input
    // is whatever operating frequency the backend reports. Band membership follows
    // cfg80211: 802.11j 4.9 GHz channels and the U-NII-4/ITS block are 5 GHz, and
    // 5925 MHz is the 5/6 GHz boundary.
    if (frequencyMhz >= 2400 && frequencyMhz <= 2500) {
      return "2.4 GHz";
    }
    if (frequencyMhz >= 4900 && frequencyMhz <= 5924) {
      return "5 GHz";
    }
    if (frequencyMhz >= 5925 && frequencyMhz <= 7125) {
      return "6 GHz";
    }
    if (frequencyMhz >= 57000 && frequencyMhz <= 71000) {
      return "60 GHz";
    }
    return nullptr;
  }

} // namespace network_display
