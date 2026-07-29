#pragma once

#include <cstdint>

struct NetworkState;

namespace network_glyphs {

  [[nodiscard]] const char* glyphForState(const NetworkState& state) noexcept;
  [[nodiscard]] const char* vpnGlyph() noexcept;
  [[nodiscard]] const char* wifiGlyphForState(const NetworkState& state) noexcept;
  [[nodiscard]] const char* wifiGlyphForSignal(std::uint8_t signal) noexcept;
  // Signal band 0 (weakest) .. 4 (strongest) — the bands the wifi glyph draws.
  // Signal-ordered UI sorts on this rather than the raw percent, which jitters
  // on every scan update.
  [[nodiscard]] int wifiSignalBand(std::uint8_t signal) noexcept;

} // namespace network_glyphs
