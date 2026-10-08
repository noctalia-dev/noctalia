#pragma once

#include "capture/screencopy_capture.h"

#include <cstdint>
#include <optional>

namespace capture {

  enum class CursorToggleStatus : std::uint8_t { Available, NotCaptured, CaptureFailed };

  struct ScreenshotImage {
    // Images are oriented to logical output coordinates at native pixel resolution.
    ScreencopyImage image;
    // An available capture needs no second image when the cursor is outside it.
    std::optional<ScreencopyImage> alternative;
    bool cursorVisible = false;
    CursorToggleStatus cursorStatus = CursorToggleStatus::NotCaptured;

    [[nodiscard]] bool canToggleCursor() const noexcept { return cursorStatus == CursorToggleStatus::Available; }

    [[nodiscard]] const ScreencopyImage& imageForCursor(bool visible) const noexcept {
      return canToggleCursor() && alternative && visible != cursorVisible ? *alternative : image;
    }
  };

  struct FrozenScreenshot {
    wl_output* output = nullptr;
    ScreenshotImage image;
  };

  struct CapturedCursor;

  [[nodiscard]] std::optional<ScreenshotImage>
  cropScreenshotImage(const ScreenshotImage& source, int logicalWidth, int logicalHeight, LogicalRect region);

  [[nodiscard]] ScreenshotImage composeCapturedCursor(ScreencopyImage background, CapturedCursor cursor);

} // namespace capture
