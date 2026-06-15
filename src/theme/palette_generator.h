#pragma once

#include "theme/palette.h"
#include "theme/scheme.h"

#include <cstdint>
#include <string>
#include <vector>

namespace noctalia::theme {

  // Top-level entry point. Accepts a forced-112×112 RGB (no alpha) pixel buffer
  // and a scheme; dispatches to the M3 (MCU-based) or custom (HSL-based) path
  // and returns a fully-populated dark+light palette.
  //
  // The buffer must contain exactly 112 * 112 * 3 bytes. Returns an empty
  // palette and writes an error message if generation fails.
  GeneratedPalette generate(
      const std::vector<uint8_t>& rgb112, const std::vector<uint32_t>& fallback_colors, Scheme scheme,
      std::string* errorMessage = nullptr
  );

  // Internal paths — exposed for unit testing / analysis tool reuse.
  GeneratedPalette
  generateMaterial(const std::vector<uint8_t>& rgb112, const std::vector<uint32_t>& fallback_colors, Scheme scheme);
  GeneratedPalette generateCustom(const std::vector<uint8_t>& rgb112, Scheme scheme);

} // namespace noctalia::theme
