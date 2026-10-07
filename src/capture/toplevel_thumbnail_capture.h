#pragma once

#include "capture/screencopy_capture.h"
#include "core/timer_manager.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

class WaylandConnection;
struct ToplevelThumbnailCapturePending;
struct ext_foreign_toplevel_handle_v1;

namespace capture {
  [[nodiscard]] std::optional<ScreencopyImage> makeToplevelThumbnail(
      std::span<const std::uint8_t> pixels, int width, int height, std::uint32_t format, int maxWidth, int maxHeight,
      std::int32_t transform
  );
}

// One-shot, bounded-size snapshots sourced directly from an ext-foreign-toplevel handle.
class ToplevelThumbnailCapture {
public:
  using CompletionCallback = std::function<void(std::optional<ScreencopyImage>, std::string error)>;

  explicit ToplevelThumbnailCapture(WaylandConnection& wayland);
  ~ToplevelThumbnailCapture();

  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] bool busy() const noexcept { return m_pending != nullptr; }
  void capture(ext_foreign_toplevel_handle_v1* handle, int maxWidth, int maxHeight, CompletionCallback onComplete);
  void cancelInFlight();

private:
  friend struct ToplevelThumbnailCapturePending;
  void fail(std::string message);
  void finish(ScreencopyImage image);

  WaylandConnection& m_wayland;
  std::unique_ptr<ToplevelThumbnailCapturePending> m_pending;
  CompletionCallback m_onComplete;
  Timer m_timeout;
};
