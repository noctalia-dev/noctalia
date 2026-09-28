#include "capture/screencopy_util.h"

#include "capture/screencopy_capture.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

namespace {

  constexpr auto kBlockingCaptureTimeout = std::chrono::milliseconds(500);

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
    const std::int32_t transform =
        captureNeedsOutputTransform(image, output) ? output.transform : WL_OUTPUT_TRANSFORM_NORMAL;
    screencopy::orientCaptureForTransform(image, transform);
  }

} // namespace

namespace screencopy {

  void orientCaptureForTransform(ScreencopyImage& image, std::int32_t transform) {
    if (image.yInvert) {
      flipRgbaVertical(image);
      image.yInvert = false;
    }
    applyOutputTransform(image, transform);
  }

  void transformCapture(ScreencopyImage& image, std::int32_t transform) { applyOutputTransform(image, transform); }

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

    wl_display* display = wayland.display();
    const auto deadline = std::chrono::steady_clock::now() + kBlockingCaptureTimeout;
    while (!finished && capture.busy()) {
      if (wl_display_dispatch_pending(display) < 0) {
        capture.cancelInFlight();
        error = "Wayland dispatch failed";
        return false;
      }
      if (finished || !capture.busy()) {
        break;
      }

      while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) {
          capture.cancelInFlight();
          error = "Wayland dispatch failed";
          return false;
        }
        if (finished || !capture.busy()) {
          break;
        }
      }
      if (finished || !capture.busy()) {
        break;
      }

      const int flushResult = wl_display_flush(display);
      if (flushResult < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display);
        capture.cancelInFlight();
        error = "Wayland flush failed";
        return false;
      }

      pollfd fd{
          .fd = wl_display_get_fd(display),
          .events = static_cast<short>(POLLIN | (flushResult < 0 ? POLLOUT : 0)),
          .revents = 0,
      };
      int ready = 0;
      while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          ready = 0;
          break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        ready = poll(&fd, 1, std::max(1, static_cast<int>(remaining.count())));
        if (ready >= 0 || errno != EINTR) {
          break;
        }
      }

      if (ready <= 0) {
        wl_display_cancel_read(display);
        capture.cancelInFlight();
        error = ready == 0 ? "screencopy capture timed out" : "Wayland poll failed";
        return false;
      }
      if ((fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        wl_display_cancel_read(display);
        capture.cancelInFlight();
        error = "Wayland connection failed during screencopy";
        return false;
      }
      if ((fd.revents & POLLIN) != 0) {
        if (wl_display_read_events(display) < 0) {
          capture.cancelInFlight();
          error = "Wayland event read failed";
          return false;
        }
      } else {
        wl_display_cancel_read(display);
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
