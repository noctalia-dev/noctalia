#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <cstdint>
#include <functional>

class WaylandConnection;
struct wl_buffer;
struct wl_output;
struct wl_surface;
struct zwlr_layer_surface_v1;

// Maps a throwaway 1x1 fully-transparent layer surface with no output assigned,
// so the compositor places it on the output it would pick for any NULL-output
// surface — the focused one. The output is read back from the surface's first
// wl_surface.enter and reported via the callback, then the probe is torn down.
//
// This exists for compositors that expose no focus query (no IPC, no
// focused-output backend), where CompositorPlatform::focusedInteractiveOutput()
// returns nullptr. It converts "we don't know the focused output" into "ask the
// compositor" without giving up any panel placement features: the caller opens
// the real panel on the concrete output the probe reports.
//
// Single-shot: the callback fires exactly once — with the entered output, or
// nullptr if no enter arrives before the timeout. The callback may destroy this
// probe (finish() invokes it from a local copy, so that is safe).
class OutputProbe {
public:
  using Callback = std::function<void(wl_output*)>;

  OutputProbe(WaylandConnection& wayland, std::chrono::milliseconds timeout, Callback callback);
  ~OutputProbe();

  OutputProbe(const OutputProbe&) = delete;
  OutputProbe& operator=(const OutputProbe&) = delete;

  // C-callback bridges for the layer-surface and wl_surface listeners.
  static void handleConfigure(
      void* data, zwlr_layer_surface_v1* layerSurface, std::uint32_t serial, std::uint32_t width, std::uint32_t height
  );
  static void handleClosed(void* data, zwlr_layer_surface_v1* layerSurface);
  static void handleEnter(void* data, wl_surface* surface, wl_output* output);

private:
  bool createBuffer();
  void teardown();
  void finish(wl_output* output);

  WaylandConnection& m_wayland;
  Callback m_callback;
  Timer m_timeoutTimer;
  wl_buffer* m_buffer = nullptr;
  wl_surface* m_surface = nullptr;
  zwlr_layer_surface_v1* m_layerSurface = nullptr;
  bool m_bufferAttached = false;
  bool m_finished = false;
};
