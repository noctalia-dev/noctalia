#pragma once

#include <algorithm>
#include <cmath>

namespace filemanager {
  inline constexpr float kFileViewDefaultScale = 1.0F;
  inline constexpr float kFileViewMinScale = 0.8F;
  inline constexpr float kFileViewMaxScale = 1.8F;
  inline constexpr float kFileViewZoomStep = 0.1F;

  inline float clampFileViewScale(float zoom) {
    if (!std::isfinite(zoom))
      return kFileViewDefaultScale;
    for (float stop : {kFileViewMinScale, kFileViewDefaultScale, kFileViewMaxScale})
      if (std::abs(zoom - stop) < 0.0001F)
        return stop;
    return std::clamp(zoom, kFileViewMinScale, kFileViewMaxScale);
  }

  struct FileViewMetrics {
    float listIcon;
    float listRowHeight;
    float gridIcon;
    float gridCellWidth;
    float gridCellHeight;
  };

  // Zoom controls artwork, while typography and metadata columns remain stable.
  // The two views reserve only the padding and label space they actually need.
  inline FileViewMetrics fileViewMetrics(float zoom) {
    zoom = clampFileViewScale(zoom);
    const float listIcon = std::round(zoom < 1.0F ? 22.0F + (zoom - 1.0F) * 30.0F : 22.0F + (zoom - 1.0F) * 42.5F);
    const float gridIcon = std::round(48.0F + (zoom - 1.0F) * 80.0F);
    return {
        listIcon, std::max(32.0F, listIcon + 10.0F), gridIcon, std::max(112.0F, gridIcon + 48.0F), gridIcon + 52.0F
    };
  }
} // namespace filemanager
