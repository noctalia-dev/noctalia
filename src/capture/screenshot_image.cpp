#include "capture/screenshot_image.h"

#include "capture/pointer_cursor_capture.h"
#include "capture/screencopy_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <wayland-client-protocol.h>

namespace capture {

  namespace {
    [[nodiscard]] std::optional<ScreencopyImage>
    cropCapture(const ScreencopyImage& source, int logicalOutputWidth, int logicalOutputHeight, LogicalRect region) {
      if (logicalOutputWidth <= 0 || logicalOutputHeight <= 0 || region.width <= 0 || region.height <= 0) {
        return std::nullopt;
      }

      const double scaleX = static_cast<double>(source.width) / static_cast<double>(logicalOutputWidth);
      const double scaleY = static_cast<double>(source.height) / static_cast<double>(logicalOutputHeight);

      LogicalRect clipped = region;
      clipped.x = std::clamp(region.x, 0, logicalOutputWidth);
      clipped.y = std::clamp(region.y, 0, logicalOutputHeight);
      clipped.width = std::clamp(region.width, 0, logicalOutputWidth - clipped.x);
      clipped.height = std::clamp(region.height, 0, logicalOutputHeight - clipped.y);
      if (clipped.width <= 0 || clipped.height <= 0) {
        return std::nullopt;
      }

      const int srcX0 = std::clamp(static_cast<int>(std::floor(clipped.x * scaleX)), 0, source.width);
      const int srcY0 = std::clamp(static_cast<int>(std::floor(clipped.y * scaleY)), 0, source.height);
      const int srcX1 = std::clamp(static_cast<int>(std::ceil((clipped.x + clipped.width) * scaleX)), 0, source.width);
      const int srcY1 =
          std::clamp(static_cast<int>(std::ceil((clipped.y + clipped.height) * scaleY)), 0, source.height);
      const int outWidth = srcX1 - srcX0;
      const int outHeight = srcY1 - srcY0;
      if (outWidth <= 0 || outHeight <= 0) {
        return std::nullopt;
      }

      ScreencopyImage cropped;
      cropped.width = outWidth;
      cropped.height = outHeight;
      cropped.rgba.resize(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * 4U);

      for (int y = 0; y < outHeight; ++y) {
        const int srcY = srcY0 + y;
        const auto* srcRow = source.rgba.data()
            + (static_cast<std::size_t>(srcY) * static_cast<std::size_t>(source.width)
               + static_cast<std::size_t>(srcX0))
                * 4U;
        auto* dstRow = cropped.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth) * 4U;
        std::memcpy(dstRow, srcRow, static_cast<std::size_t>(outWidth) * 4U);
      }

      return cropped;
    }
  } // namespace
  [[nodiscard]] std::optional<capture::ScreenshotImage>
  cropScreenshotImage(const capture::ScreenshotImage& source, int logicalWidth, int logicalHeight, LogicalRect region) {
    auto image = cropCapture(source.image, logicalWidth, logicalHeight, region);
    if (!image) {
      return std::nullopt;
    }
    capture::ScreenshotImage cropped{
        .image = std::move(*image),
        .cursorVisible = source.cursorVisible,
        .cursorStatus = source.cursorStatus,
    };
    if (source.alternative) {
      cropped.alternative = cropCapture(*source.alternative, logicalWidth, logicalHeight, region);
      if (!cropped.alternative) {
        cropped.cursorStatus = capture::CursorToggleStatus::CaptureFailed;
      }
    }
    return cropped;
  }

  ScreenshotImage composeCapturedCursor(ScreencopyImage background, CapturedCursor cursor) {
    const int width = cursor.image.width;
    if (!cursor.visible) {
      return ScreenshotImage{.image = std::move(background), .cursorStatus = CursorToggleStatus::Available};
    }
    const int height = cursor.image.height;
    const int x = cursor.hotspotX;
    const int y = cursor.hotspotY;
    switch (cursor.transform) {
    case WL_OUTPUT_TRANSFORM_90:
      cursor.hotspotX = height - y;
      cursor.hotspotY = x;
      break;
    case WL_OUTPUT_TRANSFORM_180:
      cursor.hotspotX = width - x;
      cursor.hotspotY = height - y;
      break;
    case WL_OUTPUT_TRANSFORM_270:
      cursor.hotspotX = y;
      cursor.hotspotY = width - x;
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
      cursor.hotspotX = width - x;
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      cursor.hotspotX = height - y;
      cursor.hotspotY = width - x;
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      cursor.hotspotY = height - y;
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      cursor.hotspotX = y;
      cursor.hotspotY = x;
      break;
    default:
      break;
    }
    screencopy::transformCapture(cursor.image, static_cast<std::int32_t>(cursor.transform));
    ScreenshotImage result{
        .image = std::move(background),
        .cursorStatus = CursorToggleStatus::Available,
    };
    result.alternative = result.image;
    auto& painted = *result.alternative;
    const std::int64_t left = static_cast<std::int64_t>(cursor.x) - cursor.hotspotX;
    const std::int64_t top = static_cast<std::int64_t>(cursor.y) - cursor.hotspotY;
    const auto x0 = static_cast<int>(std::clamp<std::int64_t>(left, 0, painted.width));
    const auto y0 = static_cast<int>(std::clamp<std::int64_t>(top, 0, painted.height));
    const auto x1 = static_cast<int>(std::clamp<std::int64_t>(left + cursor.image.width, 0, painted.width));
    const auto y1 = static_cast<int>(std::clamp<std::int64_t>(top + cursor.image.height, 0, painted.height));
    for (int destY = y0; destY < y1; ++destY) {
      for (int destX = x0; destX < x1; ++destX) {
        const auto* src = cursor.image.rgba.data()
            + (static_cast<std::size_t>(destY - top) * static_cast<std::size_t>(cursor.image.width)
               + static_cast<std::size_t>(destX - left))
                * 4U;
        if (src[3] == 0) {
          continue;
        }
        auto* dst = painted.rgba.data()
            + (static_cast<std::size_t>(destY) * static_cast<std::size_t>(painted.width)
               + static_cast<std::size_t>(destX))
                * 4U;
        const unsigned alpha = src[3];
        const unsigned remaining = dst[3] * (255U - alpha);
        const unsigned combined = alpha * 255U + remaining;
        for (int channel = 0; channel < 3; ++channel) {
          dst[channel] = static_cast<std::uint8_t>(
              (src[channel] * alpha * 255U + dst[channel] * remaining + combined / 2U) / combined
          );
        }
        dst[3] = static_cast<std::uint8_t>((combined + 127U) / 255U);
      }
    }
    return result;
  }

} // namespace capture
