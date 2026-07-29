#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

struct LoadedImageFile {
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
  // Decoded dimensions before any crop/resize (SVG: rasterized size).
  int sourceWidth = 0;
  int sourceHeight = 0;
};

[[nodiscard]] std::expected<LoadedImageFile, std::string>
loadImageFile(const std::string& path, int targetSize = 0, bool centerSquareCrop = false);
