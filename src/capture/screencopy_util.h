#pragma once

#include <cstdint>
#include <string>

struct wl_output;
class ScreencopyCapture;
struct ScreencopyImage;
class WaylandConnection;

namespace screencopy {

  [[nodiscard]] bool captureOutputBlocking(
      ScreencopyCapture& capture, WaylandConnection& wayland, wl_output* output, ScreencopyImage& out,
      std::string& error, bool overlayCursor = false
  );

  [[nodiscard]] bool orientCaptureNative(ScreencopyImage& image, const WaylandConnection& wayland, wl_output* output);

  void transformCapture(ScreencopyImage& image, std::int32_t transform);

} // namespace screencopy
