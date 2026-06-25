#pragma once

#include <string>

struct wl_output;
class ScreencopyCapture;
struct ScreencopyImage;
class WaylandConnection;

namespace screencopy {

  [[nodiscard]] bool captureOutputBlocking(
      ScreencopyCapture& capture, WaylandConnection& wayland, wl_output* output, ScreencopyImage& out,
      std::string& error
  );

  [[nodiscard]] bool orientCaptureNative(ScreencopyImage& image, const WaylandConnection& wayland, wl_output* output);

} // namespace screencopy
