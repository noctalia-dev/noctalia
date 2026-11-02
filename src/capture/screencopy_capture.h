#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct wl_output;
class WaylandConnection;

struct LogicalRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct ScreencopyImage {
  int width = 0;
  int height = 0;
  bool yInvert = false;
  std::vector<std::uint8_t> rgba;
};

struct ScreencopyCapturePending;

class ScreencopyCapture {
public:
  using CompletionCallback = std::function<void(std::optional<ScreencopyImage>, std::string error)>;

  explicit ScreencopyCapture(WaylandConnection& wayland);
  ~ScreencopyCapture();

  bool available() const noexcept;
  bool busy() const noexcept { return m_busy; }
  WaylandConnection& wayland() noexcept { return m_wayland; }

  void capture(wl_output* output, std::optional<LogicalRect> region, bool overlayCursor, CompletionCallback onComplete);
  void cancelInFlight();

  void fail(std::string message);
  void finish(ScreencopyImage image);
  void destroyPending();

private:
  WaylandConnection& m_wayland;
  bool m_busy = false;
  CompletionCallback m_onComplete;
  std::unique_ptr<ScreencopyCapturePending> m_pending;
};
