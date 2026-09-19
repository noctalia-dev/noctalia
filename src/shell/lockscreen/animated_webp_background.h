#pragma once

#include "core/timer_manager.h"
#include "render/core/texture_handle.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RenderContext;
struct WebPAnimDecoder;

/// Streams an animated WebP onto a single, reused GPU texture: one frame is
/// decoded per timer tick (paced by the frame's own duration) and uploaded in
/// place via TextureManager::updateSubImage, so VRAM stays at ~one frame
/// regardless of clip length. Used as the lockscreen background when the
/// configured `[lockscreen] wallpaper` path is an animated WebP.
///
/// All work happens on the main loop thread (timer callbacks, GL uploads); the
/// class is not thread-safe by design.
class AnimatedWebpBackground {
public:
  enum class StartResult {
    NotAnimated,      ///< not a WebP, a still WebP, or an oversized canvas -> use the static path (and don't retry)
    TransientFailure, ///< a readable animated WebP but a transient error (e.g. lost GL context) -> retry later
    Started,          ///< streaming
  };

  AnimatedWebpBackground() = default;
  ~AnimatedWebpBackground();

  AnimatedWebpBackground(const AnimatedWebpBackground&) = delete;
  AnimatedWebpBackground& operator=(const AnimatedWebpBackground&) = delete;

  /// Begins playback if `path` is an animated WebP within the canvas-size cap.
  /// `onFrame` fires after each uploaded frame (rebind the node texture + redraw).
  /// `onFailure` fires once if a mid-stream decode/GPU error stops playback, so
  /// the owner can drop the now-deleted texture and fall back to a static path.
  /// The distinction in the return value lets the caller retry transient errors
  /// while permanently ruling out still/broken files.
  StartResult
  start(RenderContext& ctx, const std::string& path, std::function<void()> onFrame, std::function<void()> onFailure);

  /// Full teardown: stops the timer, releases the GPU texture, frees the decoder.
  /// Safe when inactive. Must run while `ctx` (from start) is still valid.
  void stop();

  /// Suspend/resume decoding without tearing down (e.g. DPMS blank / unblank).
  void pause();
  void resume();

  /// Forget the GPU texture after a graphics-context loss without touching GL
  /// (the name is already gone). The next frame recreates it. Pair with kick()
  /// once the context is restored. Does NOT rewind the decoder or its pacing.
  void invalidateGpu();

  /// Decode and upload the next frame immediately, then resume pacing. Used to
  /// repaint after a GPU reset. No-op when inactive or paused.
  void kick();

  [[nodiscard]] bool active() const noexcept { return m_decoder != nullptr; }
  [[nodiscard]] const std::string& path() const noexcept { return m_path; }
  [[nodiscard]] TextureHandle texture() const noexcept { return m_texture; }

private:
  enum class FrameStatus {
    Ok,          ///< a frame was decoded and uploaded
    ContextLost, ///< GL context not current / texture alloc failed -> retry, keep the decoder
    DecodeError, ///< the bitstream failed to decode -> stop (permanent for this file)
  };

  struct DecoderDeleter {
    void operator()(WebPAnimDecoder* decoder) const noexcept;
  };

  bool ensureTexture();
  FrameStatus pumpFrame(int& outDelayMs);
  void advanceAndUpload();
  void scheduleNext(int delayMs);
  void failStop();
  void releaseTexture();

  RenderContext* m_ctx = nullptr;
  std::string m_path;
  std::vector<std::uint8_t> m_bytes; // owns the encoded data the decoder references
  std::unique_ptr<WebPAnimDecoder, DecoderDeleter> m_decoder;
  int m_width = 0;
  int m_height = 0;
  int m_prevTimestampMs = 0; // cumulative end time of the last shown frame (libwebp semantics)
  bool m_paused = false;
  TextureHandle m_texture{};
  Timer m_frameTimer;
  std::function<void()> m_onFrame;
  std::function<void()> m_onFailure;
};
