#include "shell/wallpaper/wallpaper_geometry.h"

#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cstdint>

WallpaperSpanParams computeWallpaperSpanParams(const std::vector<WaylandOutput>& outputs, std::uint32_t outputName) {
  WallpaperSpanParams span;
  bool haveBounds = false;
  std::int32_t minX = 0;
  std::int32_t minY = 0;
  std::int32_t maxX = 0;
  std::int32_t maxY = 0;
  const WaylandOutput* self = nullptr;

  for (const auto& output : outputs) {
    if (!output.done || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
      continue;
    }
    const std::int32_t left = output.logicalX;
    const std::int32_t top = output.logicalY;
    const std::int32_t right = output.logicalX + output.logicalWidth;
    const std::int32_t bottom = output.logicalY + output.logicalHeight;
    if (!haveBounds) {
      minX = left;
      minY = top;
      maxX = right;
      maxY = bottom;
      haveBounds = true;
    } else {
      minX = std::min(minX, left);
      minY = std::min(minY, top);
      maxX = std::max(maxX, right);
      maxY = std::max(maxY, bottom);
    }
    if (output.name == outputName) {
      self = &output;
    }
  }

  if (!haveBounds || self == nullptr) {
    return span;
  }

  span.offsetX = static_cast<float>(self->logicalX - minX);
  span.offsetY = static_cast<float>(self->logicalY - minY);
  span.monitorWidth = static_cast<float>(self->logicalWidth);
  span.monitorHeight = static_cast<float>(self->logicalHeight);
  span.totalWidth = static_cast<float>(maxX - minX);
  span.totalHeight = static_cast<float>(maxY - minY);
  return span;
}
