#pragma once

#include "config/config_types.h"
#include "render/core/shared_texture_cache.h"
#include "render/core/wallpaper_types.h"

#include <cstdint>
#include <vector>

struct WaylandOutput;

[[nodiscard]] WallpaperSpanParams
computeWallpaperSpanParams(const std::vector<WaylandOutput>& outputs, std::uint32_t outputName);

// Physical pixels a wallpaper drawn on `outputName` with `fillMode` must keep. Crop, fit and stretch sample the image
// at no finer than the output's size, span at the desktop's; center and repeat draw source pixels 1:1 and keep the
// full resolution.
[[nodiscard]] TextureCoverage wallpaperTextureCoverage(
    const std::vector<WaylandOutput>& outputs, std::uint32_t outputName, WallpaperFillMode fillMode
);
