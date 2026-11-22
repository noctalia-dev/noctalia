#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace capture {

  // Pixels along one axis for a logical span [lo, hi), matching the crop's floor/ceil/clamp
  // bounds so the badge reports the saved file's numbers.
  constexpr int deviceSelectionExtent(int lo, int hi, double scale, int frameExtent) {
    const int start = std::clamp(static_cast<int>(std::floor(lo * scale)), 0, frameExtent);
    const int end = std::clamp(static_cast<int>(std::ceil(hi * scale)), 0, frameExtent);
    return end - start;
  }

  // Pixels per logical unit: the mode/logical ratio the crop derives, else the resolved output
  // scale. Buffer scale misses viewporter-scaled compositors and wl_output.scale misses fractions.
  constexpr double devicePixelsPerLogicalUnit(int modeExtent, int logicalExtent, double resolvedOutputScale) {
    if (modeExtent > 0 && logicalExtent > 0) {
      return static_cast<double>(modeExtent) / static_cast<double>(logicalExtent);
    }
    return resolvedOutputScale > 1.0 ? resolvedOutputScale : 1.0;
  }

  // One touched output: the crop it contributes to the file, and that output's oriented mode and
  // logical extents, which is what canvas density is measured from.
  struct RegionPieceRatio {
    int cropWidth = 0;
    int cropHeight = 0;
    int outputWidth = 0;
    int outputLogicalWidth = 0;
    int outputHeight = 0;
    int outputLogicalHeight = 0;
    double outputScale = 1.0;
  };

  struct RegionCanvasSize {
    int width = 0;
    int height = 0;
  };

  // The saved file's dimensions mirror composeGlobalRegion(). One output's crop passes through,
  // several compose onto a canvas at the sharpest output's mode/logical ratio.
  constexpr RegionCanvasSize
  regionCanvasSize(int selectionWidth, int selectionHeight, const RegionPieceRatio* pieces, std::size_t pieceCount) {
    if (pieceCount == 1) {
      return {pieces[0].cropWidth, pieces[0].cropHeight};
    }
    if (pieceCount == 0) {
      return {};
    }
    double density = 1.0;
    for (std::size_t i = 0; i < pieceCount; ++i) {
      density = std::max(
          density,
          devicePixelsPerLogicalUnit(pieces[i].outputWidth, pieces[i].outputLogicalWidth, pieces[i].outputScale)
      );
      density = std::max(
          density,
          devicePixelsPerLogicalUnit(pieces[i].outputHeight, pieces[i].outputLogicalHeight, pieces[i].outputScale)
      );
    }
    return {
        static_cast<int>(std::ceil(static_cast<double>(selectionWidth) * density)),
        static_cast<int>(std::ceil(static_cast<double>(selectionHeight) * density))
    };
  }

} // namespace capture
