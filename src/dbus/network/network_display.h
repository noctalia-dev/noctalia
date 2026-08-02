#pragma once

#include <cstdint>

struct NetworkState;

namespace network_display {

  [[nodiscard]] const char* glyphForState(const NetworkState& state) noexcept;
  [[nodiscard]] const char* vpnGlyph() noexcept;
  [[nodiscard]] const char* wifiGlyphForState(const NetworkState& state) noexcept;
  [[nodiscard]] const char* wifiGlyphForSignal(std::uint8_t signal) noexcept;
  // Signal band 0 (weakest) .. 4 (strongest) — the bands the wifi glyph draws.
  // Signal-ordered UI sorts on this rather than the raw percent, which jitters
  // on every scan update.
  [[nodiscard]] int wifiSignalBand(std::uint8_t signal) noexcept;

  // Radio band of an operating frequency: "2.4 GHz", "5 GHz", "6 GHz" or "60 GHz".
  // nullptr when the frequency is 0 (backends such as iwd never report one) or
  // falls outside those bands — 802.11y 3.6 GHz and S1G 900 MHz have no label here.
  [[nodiscard]] const char* wifiFrequencyBandLabel(std::uint32_t frequencyMhz) noexcept;

} // namespace network_display
