#include "shell/lockscreen/animated_webp_background.h"

#include "core/log.h"
#include "render/backend/render_backend.h"
#include "render/core/image_decoder.h" // kMaxWebpCanvasBytes
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <webp/decode.h>
#include <webp/demux.h>

namespace {

  constexpr Logger kLog("lockscreen");

  // Floor on a frame's on-screen time. Guards against a 0ms (or malformed
  // negative) inter-frame delta spinning the timer, and keeps the cadence at or
  // under one display refresh (~60Hz) so decode+upload cannot starve input.
  constexpr int kMinFrameDelayMs = 16;

} // namespace

void AnimatedWebpBackground::DecoderDeleter::operator()(WebPAnimDecoder* decoder) const noexcept {
  WebPAnimDecoderDelete(decoder);
}

AnimatedWebpBackground::~AnimatedWebpBackground() { stop(); }

AnimatedWebpBackground::StartResult AnimatedWebpBackground::start(
    RenderContext& ctx, const std::string& path, std::function<void()> onFrame, std::function<void()> onFailure
) {
  stop();

  std::vector<std::uint8_t> bytes = FileUtils::readBinaryFile(path);
  if (bytes.empty()) {
    return StartResult::TransientFailure; // missing/unreadable now; may appear later
  }

  WebPBitstreamFeatures features;
  if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK) {
    return StartResult::NotAnimated; // not a WebP
  }
  if (features.has_animation == 0) {
    return StartResult::NotAnimated; // still WebP -> static path
  }
  // Reject an oversized canvas BEFORE WebPAnimDecoderNew: a VP8X header can
  // declare dimensions independent of the (small) file size, and the decoder
  // would allocate the full canvas twice up front (an OOM vector).
  if (features.width <= 0
      || features.height <= 0
      || static_cast<std::uint64_t>(features.width) * static_cast<std::uint64_t>(features.height) * 4
          > kMaxWebpCanvasBytes) {
    kLog.warn("animated WebP \"{}\" canvas {}x{} exceeds size cap; ignoring", path, features.width, features.height);
    return StartResult::NotAnimated; // treat like a still image: fall back, do not stream
  }

  WebPAnimDecoderOptions options;
  if (WebPAnimDecoderOptionsInit(&options) == 0) {
    return StartResult::TransientFailure;
  }
  options.color_mode = MODE_RGBA; // non-premultiplied RGBA8, matches TextureDataFormat::Rgba
  options.use_threads = 0;

  // The decoder references the encoded bytes without copying them, so they must
  // outlive it. Build decoder + bytes as locals and only commit to members once
  // every probe has passed, so the failure paths above need no cleanup.
  const WebPData webpData{.bytes = bytes.data(), .size = bytes.size()};
  std::unique_ptr<WebPAnimDecoder, DecoderDeleter> decoder(WebPAnimDecoderNew(&webpData, &options));
  if (!decoder) {
    kLog.warn("failed to open WebP animation \"{}\"", path);
    return StartResult::NotAnimated;
  }

  WebPAnimInfo info;
  if (WebPAnimDecoderGetInfo(decoder.get(), &info) == 0 || info.canvas_width == 0 || info.canvas_height == 0) {
    kLog.warn("failed to read WebP animation info \"{}\"", path);
    return StartResult::NotAnimated;
  }

  m_ctx = &ctx;
  m_path = path;
  m_bytes = std::move(bytes);
  m_decoder = std::move(decoder);
  m_width = static_cast<int>(info.canvas_width);
  m_height = static_cast<int>(info.canvas_height);
  m_prevTimestampMs = 0;
  m_paused = false;
  m_onFrame = std::move(onFrame);
  m_onFailure = std::move(onFailure);

  kLog.info("streaming animated WebP background \"{}\" ({}x{}, {} frames)", path, m_width, m_height, info.frame_count);

  // Show the first frame immediately, then self-schedule the rest.
  int firstDelayMs = kMinFrameDelayMs;
  switch (pumpFrame(firstDelayMs)) {
  case FrameStatus::Ok:
    scheduleNext(firstDelayMs);
    return StartResult::Started;
  case FrameStatus::ContextLost:
    stop();
    return StartResult::TransientFailure; // context not ready (e.g. mid GPU reset); caller retries
  case FrameStatus::DecodeError:
    stop();
    return StartResult::NotAnimated; // frame 0 is undecodable -> treat as a broken/still file
  }
  return StartResult::TransientFailure; // unreachable
}

void AnimatedWebpBackground::stop() {
  m_frameTimer.stop();
  releaseTexture();
  m_decoder.reset();
  m_bytes.clear();
  m_bytes.shrink_to_fit();
  m_onFrame = nullptr;
  m_onFailure = nullptr;
  m_ctx = nullptr;
  m_path.clear();
  m_width = 0;
  m_height = 0;
  m_prevTimestampMs = 0;
  m_paused = false;
}

void AnimatedWebpBackground::pause() {
  if (m_paused || !m_decoder) {
    return;
  }
  m_paused = true;
  m_frameTimer.stop();
}

void AnimatedWebpBackground::resume() {
  if (!m_paused || !m_decoder) {
    return;
  }
  m_paused = false;
  advanceAndUpload();
}

void AnimatedWebpBackground::invalidateGpu() {
  m_frameTimer.stop();
  // The GL name is already gone after a context loss; forget the handle without
  // issuing a delete. ensureTexture() recreates it on the next frame. Do NOT
  // touch m_prevTimestampMs: the decoder position is unchanged, so the previous
  // frame's end time is still the correct pacing baseline.
  m_texture = {};
}

void AnimatedWebpBackground::kick() {
  if (!m_decoder || m_paused) {
    return;
  }
  advanceAndUpload();
}

bool AnimatedWebpBackground::ensureTexture() {
  if (m_texture.id.valid()) {
    return true;
  }
  if (m_ctx == nullptr) {
    return false;
  }
  m_texture = m_ctx->textureManager().createEmpty(m_width, m_height, TextureDataFormat::Rgba, TextureFilter::Linear);
  return m_texture.id.valid();
}

AnimatedWebpBackground::FrameStatus AnimatedWebpBackground::pumpFrame(int& outDelayMs) {
  if (m_ctx == nullptr || !m_decoder) {
    return FrameStatus::DecodeError;
  }
  // Timer-driven GL work must make the context current itself; during a graphics
  // reset it is not, and we retry rather than deleting the texture.
  if (!m_ctx->backend().makeCurrentNoSurface()) {
    return FrameStatus::ContextLost;
  }
  if (!ensureTexture()) {
    return FrameStatus::ContextLost;
  }

  // Loop: rewind to the first frame once the clip is exhausted.
  if (WebPAnimDecoderHasMoreFrames(m_decoder.get()) == 0) {
    WebPAnimDecoderReset(m_decoder.get());
    m_prevTimestampMs = 0;
  }

  std::uint8_t* frame = nullptr;
  int timestampMs = 0;
  if (WebPAnimDecoderGetNext(m_decoder.get(), &frame, &timestampMs) == 0 || frame == nullptr) {
    return FrameStatus::DecodeError;
  }

  m_ctx->textureManager().updateSubImage(m_texture, frame, 0, 0, m_width, m_height, TextureDataFormat::Rgba);

  // The returned timestamp is the frame's cumulative END time; the delta from the
  // previous end is how long this frame should stay on screen.
  outDelayMs = std::max(timestampMs - m_prevTimestampMs, kMinFrameDelayMs);
  m_prevTimestampMs = timestampMs;

  if (m_onFrame) {
    m_onFrame();
  }
  return FrameStatus::Ok;
}

void AnimatedWebpBackground::advanceAndUpload() {
  if (!m_decoder || m_paused) {
    return;
  }
  int delayMs = kMinFrameDelayMs;
  switch (pumpFrame(delayMs)) {
  case FrameStatus::Ok:
    scheduleNext(delayMs);
    break;
  case FrameStatus::ContextLost:
    scheduleNext(kMinFrameDelayMs); // retry shortly; keep the decoder and texture intent
    break;
  case FrameStatus::DecodeError:
    failStop();
    break;
  }
}

void AnimatedWebpBackground::scheduleNext(int delayMs) {
  if (!m_decoder || m_paused) {
    return;
  }
  m_frameTimer.start(std::chrono::milliseconds(delayMs), [this] { advanceAndUpload(); });
}

void AnimatedWebpBackground::failStop() {
  kLog.warn("WebP animation decode failed, stopping playback \"{}\"", m_path);
  // Capture the callback before stop() clears it, then notify the owner so it can
  // drop the now-deleted texture and fall back to a static background.
  auto onFailure = m_onFailure;
  stop();
  if (onFailure) {
    onFailure();
  }
}

void AnimatedWebpBackground::releaseTexture() {
  if (m_texture.id.valid() && m_ctx != nullptr) {
    m_ctx->backend().makeCurrentNoSurface();
    m_ctx->textureManager().unload(m_texture);
  }
  m_texture = {};
}
