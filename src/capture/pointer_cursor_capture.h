#pragma once

#include "capture/screencopy_capture.h"
#include "core/timer_manager.h"

namespace capture {
  struct CapturedCursor {
    bool visible = false;
    ScreencopyImage image;
    std::uint32_t transform = 0;
    // Hotspot position in transformed whole-output buffer pixels.
    int x = 0;
    int y = 0;
    // Offset in raw cursor-buffer pixels, effective at this image's ready event.
    int hotspotX = 0;
    int hotspotY = 0;
  };
} // namespace capture

struct PointerCursorCapturePending;

class PointerCursorCapture {
public:
  using CompletionCallback = std::function<void(std::optional<capture::CapturedCursor>, std::string error)>;

  explicit PointerCursorCapture(WaylandConnection& wayland);
  ~PointerCursorCapture();

  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] bool busy() const noexcept { return m_pending != nullptr; }
  void capture(wl_output* output, CompletionCallback onComplete);
  void cancelInFlight();

private:
  friend struct PointerCursorCapturePending;
  void fail(std::string message);
  void finish(capture::CapturedCursor cursor);

  WaylandConnection& m_wayland;
  std::unique_ptr<PointerCursorCapturePending> m_pending;
  CompletionCallback m_onComplete;
  Timer m_timeout;
};
