#include "capture/screenshot_capture.h"

#include "capture/screencopy_util.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <chrono>
#include <utility>

ScreenshotCapture::ScreenshotCapture(WaylandConnection& wayland)
    : m_wayland(wayland), m_backgroundCapture(wayland), m_cursorCapture(wayland) {}

ScreenshotCapture::~ScreenshotCapture() { cancelInFlight(); }

bool ScreenshotCapture::available() const noexcept { return m_backgroundCapture.available(); }

void ScreenshotCapture::capture(
    wl_output* output, std::optional<LogicalRect> region, bool showCursor, bool retainCursor,
    CompletionCallback onComplete
) {
  if (m_busy) {
    if (onComplete) {
      onComplete(std::nullopt, "capture already in progress");
    }
    return;
  }
  m_busy = true;
  m_output = output;
  m_region = region;
  m_showCursor = showCursor;
  m_cursor.reset();
  m_cursorStatus = capture::CursorToggleStatus::NotCaptured;
  m_onComplete = std::move(onComplete);
  if (retainCursor && m_cursorCapture.available() && output != nullptr) {
    m_cursorCapture.capture(output, [this](std::optional<capture::CapturedCursor> cursor, std::string /*error*/) {
      m_cursor = std::move(cursor);
      m_cursorStatus = m_cursor ? capture::CursorToggleStatus::Available : capture::CursorToggleStatus::CaptureFailed;
      captureBackground();
    });
    return;
  }
  captureBackground();
}

void ScreenshotCapture::captureBackground() {
  m_timeout.start(std::chrono::seconds(1), [this]() {
    m_backgroundCapture.cancelInFlight();
    finish(std::nullopt, "screencopy frame timed out");
  });
  // Capturing an overlaid wlr frame would bake the cursor into every concurrent output copy.
  m_backgroundCapture.capture(
      m_output, m_cursor ? std::nullopt : m_region, m_cursor ? false : m_showCursor,
      [this](std::optional<ScreencopyImage> image, std::string error) { finish(std::move(image), std::move(error)); }
  );
}

void ScreenshotCapture::cancelInFlight() {
  m_timeout.stop();
  m_cursorCapture.cancelInFlight();
  m_backgroundCapture.cancelInFlight();
  m_cursor.reset();
  m_busy = false;
  m_onComplete = {};
}

void ScreenshotCapture::finish(std::optional<ScreencopyImage> image, std::string error) {
  m_timeout.stop();
  std::optional<capture::ScreenshotImage> result;
  if (image) {
    if (!screencopy::orientCaptureNative(*image, m_wayland, m_output)) {
      error = "Failed to orient screenshot";
    } else if (!m_cursor) {
      result = capture::ScreenshotImage{
          .image = std::move(*image),
          .cursorVisible = m_showCursor,
          .cursorStatus = m_cursorStatus,
      };
    } else {
      result = capture::composeCapturedCursor(std::move(*image), std::move(*m_cursor));
      if (m_region) {
        const auto output = std::ranges::find(m_wayland.outputs(), m_output, &WaylandOutput::output);
        if (output == m_wayland.outputs().end()) {
          result.reset();
        } else {
          result = capture::cropScreenshotImage(*result, output->logicalWidth, output->logicalHeight, *m_region);
        }
        if (!result) {
          error = "Failed to crop screenshot";
        }
      }
      if (result && m_showCursor) {
        if (result->alternative) {
          std::swap(result->image, *result->alternative);
        }
        result->cursorVisible = true;
      }
    }
  }
  m_cursor.reset();
  m_busy = false;
  auto onComplete = std::exchange(m_onComplete, {});
  if (onComplete) {
    onComplete(std::move(result), std::move(error));
  }
}
