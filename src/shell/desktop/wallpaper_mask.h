#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

struct OutputWallpaperMask {
  std::uint64_t ownerId = 0;
  std::string path;
  std::string wallpaperPath;

  bool operator==(const OutputWallpaperMask&) const = default;
};

using OutputWallpaperMaskMap = std::unordered_map<std::string, OutputWallpaperMask>;
