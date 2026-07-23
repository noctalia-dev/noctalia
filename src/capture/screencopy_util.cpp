#include "capture/screencopy_util.h"

#include "capture/screencopy_capture.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cstring>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

namespace {

  [[nodiscard]] const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
    for (const auto& entry : wayland.outputs()) {
      if (entry.output == output) {
        return &entry;
      }
    }
    return nullptr;
  }

  void flipRgbaHorizontal(ScreencopyImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
      return;
    }

    const int w = image.width;
    const int h = image.height;
    for (int y = 0; y < h; ++y) {
      auto* row = image.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4U;
      for (int x = 0; x < w / 2; ++x) {
        auto* left = row + static_cast<std::size_t>(x) * 4U;
        auto* right = row + static_cast<std::size_t>(w - 1 - x) * 4U;
        for (int c = 0; c < 4; ++c) {
          std::swap(left[c], right[c]);
        }
      }
    }
  }

  void flipRgbaVertical(ScreencopyImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
      return;
    }

    const int w = image.width;
    const int h = image.height;
    for (int y = 0; y < h / 2; ++y) {
      auto* top = image.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4U;
      auto* bottom = image.rgba.data() + static_cast<std::size_t>(h - 1 - y) * static_cast<std::size_t>(w) * 4U;
      for (int x = 0; x < w; ++x) {
        auto* topPx = top + static_cast<std::size_t>(x) * 4U;
        auto* bottomPx = bottom + static_cast<std::size_t>(x) * 4U;
        for (int c = 0; c < 4; ++c) {
          std::swap(topPx[c], bottomPx[c]);
        }
      }
    }
  }

  void rotateRgbaCw90(ScreencopyImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
      return;
    }

    const int srcW = image.width;
    const int srcH = image.height;
    const int dstW = srcH;
    const int dstH = srcW;
    std::vector<std::uint8_t> rotated(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH) * 4U);

    for (int srcY = 0; srcY < srcH; ++srcY) {
      for (int srcX = 0; srcX < srcW; ++srcX) {
        const int dstX = srcH - 1 - srcY;
        const int dstY = srcX;
        const auto* srcPx = image.rgba.data()
            + (static_cast<std::size_t>(srcY) * static_cast<std::size_t>(srcW) + static_cast<std::size_t>(srcX)) * 4U;
        auto* dstPx = rotated.data()
            + (static_cast<std::size_t>(dstY) * static_cast<std::size_t>(dstW) + static_cast<std::size_t>(dstX)) * 4U;
        std::memcpy(dstPx, srcPx, 4U);
      }
    }

    image.width = dstW;
    image.height = dstH;
    image.rgba = std::move(rotated);
  }

  void rotateRgba180(ScreencopyImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
      return;
    }

    const int w = image.width;
    const int h = image.height;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const int mirrorX = w - 1 - x;
        const int mirrorY = h - 1 - y;
        if (mirrorY < y || (mirrorY == y && mirrorX <= x)) {
          continue;
        }
        auto* a = image.rgba.data()
            + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4U;
        auto* b = image.rgba.data()
            + (static_cast<std::size_t>(mirrorY) * static_cast<std::size_t>(w) + static_cast<std::size_t>(mirrorX))
                * 4U;
        for (int c = 0; c < 4; ++c) {
          std::swap(a[c], b[c]);
        }
      }
    }
  }

  void rotateRgbaCw270(ScreencopyImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
      return;
    }

    const int srcW = image.width;
    const int srcH = image.height;
    const int dstW = srcH;
    const int dstH = srcW;
    std::vector<std::uint8_t> rotated(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH) * 4U);

    for (int srcY = 0; srcY < srcH; ++srcY) {
      for (int srcX = 0; srcX < srcW; ++srcX) {
        const int dstX = srcY;
        const int dstY = srcW - 1 - srcX;
        const auto* srcPx = image.rgba.data()
            + (static_cast<std::size_t>(srcY) * static_cast<std::size_t>(srcW) + static_cast<std::size_t>(srcX)) * 4U;
        auto* dstPx = rotated.data()
            + (static_cast<std::size_t>(dstY) * static_cast<std::size_t>(dstW) + static_cast<std::size_t>(dstX)) * 4U;
        std::memcpy(dstPx, srcPx, 4U);
      }
    }

    image.width = dstW;
    image.height = dstH;
    image.rgba = std::move(rotated);
  }

  void applyOutputTransform(ScreencopyImage& image, std::int32_t transform) {
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_90:
      rotateRgbaCw90(image);
      break;
    case WL_OUTPUT_TRANSFORM_180:
      rotateRgba180(image);
      break;
    case WL_OUTPUT_TRANSFORM_270:
      rotateRgbaCw270(image);
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
      flipRgbaHorizontal(image);
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      flipRgbaHorizontal(image);
      rotateRgbaCw90(image);
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      flipRgbaVertical(image);
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      flipRgbaHorizontal(image);
      rotateRgbaCw270(image);
      break;
    default:
      break;
    }
  }

  [[nodiscard]] bool captureNeedsOutputTransform(const ScreencopyImage& image, const WaylandOutput& output) {
    if (output.transform == WL_OUTPUT_TRANSFORM_NORMAL) {
      return false;
    }

    const bool dimsMatchLogical = image.width == output.logicalWidth && image.height == output.logicalHeight;
    const bool dimsMatchPhysical =
        output.width > 0 && output.height > 0 && image.width == output.width && image.height == output.height;
    if (dimsMatchLogical && !dimsMatchPhysical) {
      return false;
    }
    return true;
  }

  void orientCaptureToLogical(ScreencopyImage& image, const WaylandOutput& output) {
    if (captureNeedsOutputTransform(image, output)) {
      applyOutputTransform(image, output.transform);
    }
    if (image.yInvert) {
      flipRgbaVertical(image);
      image.yInvert = false;
    }
  }

} // namespace

namespace screencopy {

  bool captureOutputBlocking(
      ScreencopyCapture& capture, WaylandConnection& wayland, wl_output* output, ScreencopyImage& out,
      std::string& error, bool overlayCursor
  ) {
    error.clear();
    bool finished = false;
    capture.capture(
        output, std::nullopt, overlayCursor, [&](std::optional<ScreencopyImage> image, const std::string& err) {
          finished = true;
          if (!err.empty() || !image.has_value()) {
            error = err.empty() ? "screencopy capture failed" : err;
            return;
          }
          out = std::move(*image);
        }
    );

    if (!error.empty()) {
      return false;
    }

    while (!finished && capture.busy()) {
      if (wl_display_roundtrip(wayland.display()) < 0) {
        error = "Wayland roundtrip failed";
        return false;
      }
    }

    if (!error.empty() || !finished) {
      if (error.empty()) {
        error = "screencopy capture failed";
      }
      return false;
    }

    if (out.width <= 0 || out.height <= 0 || out.rgba.empty()) {
      error = "screencopy capture returned an empty frame";
      return false;
    }

    return true;
  }

  bool orientCaptureNative(ScreencopyImage& image, const WaylandConnection& wayland, wl_output* output) {
    const WaylandOutput* out = findOutput(wayland, output);
    if (out == nullptr) {
      return false;
    }
    orientCaptureToLogical(image, *out);
    return true;
  }

} // namespace screencopy
