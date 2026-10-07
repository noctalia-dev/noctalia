#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

struct wl_output;
class ScreencopyCapture;
struct ScreencopyImage;
class WaylandConnection;

namespace screencopy {

  inline constexpr auto kBlockingCaptureTimeout = std::chrono::milliseconds(500);

  enum class CaptureOutputStatus : std::uint8_t {
    Success,
    CaptureFailed,
    EventLoopFailed,
  };

  [[nodiscard]] CaptureOutputStatus captureOutputBlocking(
      ScreencopyCapture& capture, WaylandConnection& wayland, wl_output* output, ScreencopyImage& out,
      std::string& error, bool overlayCursor = false, WaylandConnection* eventConnection = nullptr,
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt
  );

  [[nodiscard]] bool orientCaptureNative(ScreencopyImage& image, const WaylandConnection& wayland, wl_output* output);

  void orientCaptureForTransform(ScreencopyImage& image, std::int32_t transform);

  void transformCapture(ScreencopyImage& image, std::int32_t transform);

} // namespace screencopy
