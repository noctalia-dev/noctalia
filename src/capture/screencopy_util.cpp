#include "capture/screencopy_util.h"

#include "capture/screencopy_blocking.h"
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

  // Deadline-aware wait on the Wayland fd. wl_display_roundtrip() cannot be
  // used here: it blocks until the server answers, so a compositor that never
  // delivers ready/failed for a frame (seen 2026-08-04 with lock-screen
  // snapshots on Hyprland) blocks it forever and no deadline checked around it
  // can fire. Dispatch what is already queued, then poll the fd for at most
  // the remaining budget and dispatch whatever arrived.
  //
  // Returns >0 when events were dispatched, 0 when the wait expired or was
  // interrupted with no progress, <0 on a connection error.
  [[nodiscard]] int waitAndDispatchWayland(wl_display* display, std::chrono::milliseconds timeout) {
    if (display == nullptr) {
      return -1;
    }

    if (wl_display_dispatch_pending(display) < 0) {
      return -1;
    }

    // prepare_read/read_events rather than wl_display_dispatch(): only the
    // latter pair lets us sit in our own poll() with a timeout.
    while (wl_display_prepare_read(display) != 0) {
      // prepare_read only refuses while the queue still holds events, so drain
      // them; any progress goes back to the caller, which re-checks the
      // deadline before waiting again.
      const int dispatched = wl_display_dispatch_pending(display);
      if (dispatched < 0) {
        return -1;
      }
      if (dispatched > 0) {
        return dispatched;
      }
      // Nothing drained and still cannot prepare: yield instead of spinning.
      return 0;
    }

    if (wl_display_flush(display) < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      return -1;
    }

    pollfd pollFd{.fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0};
    const int ready = ::poll(&pollFd, 1, static_cast<int>(timeout.count()));
    if (ready < 0) {
      wl_display_cancel_read(display);
      return errno == EINTR ? 0 : -1;
    }
    if (ready == 0) {
      wl_display_cancel_read(display);
      return 0;
    }

    if (wl_display_read_events(display) < 0) {
      return -1;
    }
    return wl_display_dispatch_pending(display);
  }

} // namespace

namespace screencopy {

  bool captureOutputBlocking(
      ScreencopyCapture& capture, WaylandConnection& wayland, wl_output* output, ScreencopyImage& out,
      std::string& error, bool overlayCursor
  ) {
    // The wait is bounded (see runBlockingCapture): a compositor that never
    // delivers ready/failed for this capture used to spin here forever —
    // inside the main loop, freezing the whole shell. On timeout the caller
    // falls back to the wallpaper background.
    const BlockingCaptureOps ops{
        .start = [&](
                     ScreencopyCapture::CompletionCallback onComplete
                 ) { capture.capture(output, std::nullopt, overlayCursor, std::move(onComplete)); },
        .busy = [&] { return capture.busy(); },
        .cancel = [&] { capture.cancelInFlight(); },
    };
    const EventWaitOps wait{
        .waitAndDispatch = [&](std::chrono::milliseconds timeout) {
          return waitAndDispatchWayland(wayland.display(), timeout);
        },
    };

    return runBlockingCapture(ops, wait, out, error);
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
