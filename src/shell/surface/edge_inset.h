#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace shell::surface_edge_inset {

  // Horizontal offset -> layer margin + reveal inner padding; small offsets shrink inner pad only.
  struct Horizontal {
    std::int32_t layerMargin = 0;
    float innerPadding = 0.0F;
  };

  [[nodiscard]] inline Horizontal resolve(int offsetX, float maxInnerPaddingLogical) {
    const int screenMargin = std::max(0, offsetX);
    const int maxInner = std::max(0, static_cast<int>(std::lround(maxInnerPaddingLogical)));
    if (screenMargin >= maxInner) {
      return {
          .layerMargin = screenMargin - maxInner,
          .innerPadding = static_cast<float>(maxInner),
      };
    }
    return {
        .layerMargin = 0,
        .innerPadding = static_cast<float>(screenMargin),
    };
  }

} // namespace shell::surface_edge_inset
