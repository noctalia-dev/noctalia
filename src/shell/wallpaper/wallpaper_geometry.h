#pragma once

#include "render/core/wallpaper_types.h"

#include <cstdint>
#include <vector>

struct WaylandOutput;

[[nodiscard]] WallpaperSpanParams
computeWallpaperSpanParams(const std::vector<WaylandOutput>& outputs, std::uint32_t outputName);
