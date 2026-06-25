#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::theme {

  enum class Scheme;

  // A 112×112 RGB pixel buffer ready for quantization/clustering. No alpha.
  // 112 * 112 * 3 = 37632 bytes.
  struct LoadedImage {
    std::vector<uint8_t> rgb; // size = 112 * 112 * 3
    int width = 112;
    int height = 112;
  };

  // Load `path`, decode via Wuffs, and resize to exactly 112×112 (aspect ratio
  // squashed) with alpha stripped. The resize filter is scheme-dependent:
  // triangle for M3 schemes, box for the custom schemes.
  std::expected<LoadedImage, std::string> loadAndResize(std::string_view path, Scheme scheme);

} // namespace noctalia::theme
