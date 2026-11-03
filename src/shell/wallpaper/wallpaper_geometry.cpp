#include "shell/wallpaper/wallpaper_geometry.h"

#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
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

TextureCoverage wallpaperTextureCoverage(
    const std::vector<WaylandOutput>& outputs, std::uint32_t outputName, WallpaperFillMode fillMode
) {
  const auto output = std::ranges::find(outputs, outputName, &WaylandOutput::name);
  if (output == outputs.end() || !output->hasUsableGeometry()) {
    return {};
  }

  auto width = static_cast<float>(output->effectiveLogicalWidth());
  auto height = static_cast<float>(output->effectiveLogicalHeight());
  switch (fillMode) {
  case WallpaperFillMode::Center:
  case WallpaperFillMode::Repeat:
    return {};
  case WallpaperFillMode::Span: {
    // Without span geometry the shader falls back to crop on this output.
    const WallpaperSpanParams span = computeWallpaperSpanParams(outputs, outputName);
    if (span.totalWidth > 0.0F && span.totalHeight > 0.0F) {
      width = span.totalWidth;
      height = span.totalHeight;
    }
    break;
  }
  case WallpaperFillMode::Crop:
  case WallpaperFillMode::Fit:
  case WallpaperFillMode::Stretch:
    break;
  }

  const float scale = output->configuredScale();
  return {
      .width = static_cast<int>(std::ceil(width * scale)),
      .height = static_cast<int>(std::ceil(height * scale)),
  };
}
