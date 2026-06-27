#pragma once

#include "theme/palette.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace noctalia::theme {

  enum class Scheme;

  // Top-level entry point. Accepts a forced-112×112 RGB (no alpha) pixel buffer
  // and a scheme; dispatches to the M3 (MCU-based) or custom (HSL-based) path
  // and returns a fully-populated dark+light palette.
  //
  // The buffer must contain exactly 112 * 112 * 3 bytes.
  std::expected<GeneratedPalette, std::string> generate(const std::vector<uint8_t>& rgb112, Scheme scheme);

  // Internal paths — exposed for unit testing / analysis tool reuse.
  GeneratedPalette generateMaterial(const std::vector<uint8_t>& rgb112, Scheme scheme);
  GeneratedPalette generateCustom(const std::vector<uint8_t>& rgb112, Scheme scheme);

} // namespace noctalia::theme
