#include "pipewire/sound_player.h"

#include "core/log.h"
#include "dr_wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

namespace {

  constexpr Logger kLog("sound");
  constexpr float kUiSoundGainCeiling = 0.20f;
  constexpr float kUiSoundGamma = 2.2f;

  const pw_stream_events kStreamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = SoundPlayer::onStreamStateChanged;
    events.drained = SoundPlayer::onDrained;
    events.process = SoundPlayer::onProcess;
    return events;
  }();

} // namespace

SoundPlayer::SoundPlayer(pw_loop* loop) : m_loop(loop) {}

SoundPlayer::~SoundPlayer() {
  for (auto& active : m_active) {
    if (active->listener != nullptr) {
      spa_hook_remove(active->listener);
      delete active->listener;
      active->listener = nullptr;
    }
    if (active->stream != nullptr) {
      pw_stream_disconnect(active->stream);
      pw_stream_destroy(active->stream);
      active->stream = nullptr;
    }
  }
  m_active.clear();
}

bool SoundPlayer::load(const std::string& name, const std::filesystem::path& path) {
  if (name.empty() || path.empty()) {
    return false;
  }

  drwav wav{};
  if (drwav_init_file(&wav, path.string().c_str(), nullptr) == DRWAV_FALSE) {
    kLog.warn("failed to load sound \"{}\" from {}", name, path.string());
    return false;
  }

  SoundBuffer buffer;
  buffer.sampleRate = wav.sampleRate;
  buffer.channels = std::max<std::uint32_t>(1, wav.channels);
  const std::uint64_t totalFrames = wav.totalPCMFrameCount;
  const std::size_t totalSamples = static_cast<std::size_t>(totalFrames) * static_cast<std::size_t>(buffer.channels);
  buffer.samples.resize(totalSamples);

  const std::uint64_t readFrames = drwav_read_pcm_frames_f32(&wav, totalFrames, buffer.samples.data());
  drwav_uninit(&wav);

  const std::size_t readSamples = static_cast<std::size_t>(readFrames) * static_cast<std::size_t>(buffer.channels);
  if (readSamples == 0) {
    kLog.warn("sound \"{}\" from {} has no samples", name, path.string());
    return false;
  }
  buffer.samples.resize(readSamples);

  m_buffers[name] = std::move(buffer);
  kLog.info("loaded sound \"{}\" from {}", name, path.string());
  return true;
}

void SoundPlayer::play(const std::string& name) {
  if (m_loop == nullptr || m_volume <= 0.0f) {
    return;
  }

  const auto it = m_buffers.find(name);
  if (it == m_buffers.end()) {
    return;
  }
  const SoundBuffer* buffer = &it->second;
  if (buffer->samples.empty()) {
    return;
  }

  removeFinished();

  for (const auto& active : m_active) {
    if (!active->finished && active->buffer == buffer) {
      return;
    }
  }

  auto active = std::make_unique<ActiveStream>();
  active->owner = this;
  active->buffer = buffer;
  active->listener = new spa_hook{};
  spa_zero(*active->listener);

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Notification", PW_KEY_APP_NAME,
      "Noctalia", nullptr
  );
  active->stream = pw_stream_new_simple(m_loop, "noctalia-sound", props, &kStreamEvents, active.get());
  if (active->stream == nullptr) {
    delete active->listener;
    kLog.warn("failed to create stream for sound \"{}\"", name);
    return;
  }

  pw_stream_add_listener(active->stream, active->listener, &kStreamEvents, active.get());

  std::uint8_t formatBuffer[1024];
  spa_pod_builder builder{};
  spa_pod_builder_init(&builder, formatBuffer, sizeof(formatBuffer));
  spa_audio_info_raw audioInfo{};
  audioInfo.format = SPA_AUDIO_FORMAT_F32;
  audioInfo.rate = buffer->sampleRate;
  audioInfo.channels = buffer->channels;
  const spa_pod* params[1];
  params[0] = reinterpret_cast<spa_pod*>(spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audioInfo));

  const int rc = pw_stream_connect(
      active->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1
  );
  if (rc < 0) {
    kLog.warn("failed to connect stream for sound \"{}\": {}", name, spa_strerror(rc));
    spa_hook_remove(active->listener);
    delete active->listener;
    pw_stream_destroy(active->stream);
    return;
  }

  m_active.push_back(std::move(active));
}

void SoundPlayer::setVolume(float volume) { m_volume = std::clamp(volume, 0.0f, 1.0f); }

void SoundPlayer::onProcess(void* userdata) {
  auto* streamState = static_cast<ActiveStream*>(userdata);
  if (streamState == nullptr || streamState->owner == nullptr) {
    return;
  }
  streamState->owner->processStream(*streamState);
}

void SoundPlayer::onStreamStateChanged(
    void* userdata, pw_stream_state /*oldState*/, pw_stream_state state, const char* error
) {
  auto* streamState = static_cast<ActiveStream*>(userdata);
  if (streamState == nullptr || streamState->owner == nullptr) {
    return;
  }

  if (state == PW_STREAM_STATE_ERROR) {
    kLog.warn("sound stream error: {}", error != nullptr ? error : "unknown");
    streamState->owner->markFinished(*streamState);
  }
  if (state == PW_STREAM_STATE_UNCONNECTED) {
    streamState->owner->markFinished(*streamState);
  }
}

void SoundPlayer::onDrained(void* userdata) {
  auto* streamState = static_cast<ActiveStream*>(userdata);
  if (streamState == nullptr || streamState->owner == nullptr) {
    return;
  }
  streamState->owner->markFinished(*streamState);
}

void SoundPlayer::processStream(ActiveStream& streamState) {
  if (streamState.stream == nullptr || streamState.buffer == nullptr || streamState.finished) {
    markFinished(streamState);
    return;
  }

  pw_buffer* pwBuffer = pw_stream_dequeue_buffer(streamState.stream);
  if (pwBuffer == nullptr) {
    return;
  }

  spa_buffer* spaBuffer = pwBuffer->buffer;
  spa_data& data = spaBuffer->datas[0];
  if (data.data == nullptr || data.maxsize < sizeof(float)) {
    pw_stream_queue_buffer(streamState.stream, pwBuffer);
    return;
  }

  const auto* src = streamState.buffer->samples.data();
  const std::size_t sampleCount = streamState.buffer->samples.size();
  auto* dst = static_cast<float*>(data.data);
  const std::size_t capacitySamples = data.maxsize / sizeof(float);
  const std::size_t remaining =
      (streamState.cursor < sampleCount && !streamState.draining) ? (sampleCount - streamState.cursor) : 0;
  const std::size_t copySamples = std::min(capacitySamples, remaining);
  const float playbackGain = std::pow(m_volume, kUiSoundGamma) * kUiSoundGainCeiling;

  for (std::size_t i = 0; i < copySamples; ++i) {
    dst[i] = src[streamState.cursor + i] * playbackGain;
  }

  if (copySamples < capacitySamples) {
    std::memset(dst + copySamples, 0, (capacitySamples - copySamples) * sizeof(float));
  }

  streamState.cursor += copySamples;
  data.chunk->offset = 0;
  data.chunk->size = static_cast<std::uint32_t>(copySamples * sizeof(float));
  data.chunk->stride = static_cast<std::int32_t>(streamState.buffer->channels * sizeof(float));
  pw_stream_queue_buffer(streamState.stream, pwBuffer);

  if (streamState.cursor >= sampleCount && !streamState.draining) {
    streamState.draining = true;
    (void)pw_stream_flush(streamState.stream, true);
  }
}

void SoundPlayer::markFinished(ActiveStream& streamState) { streamState.finished = true; }

void SoundPlayer::removeFinished() {
  std::erase_if(m_active, [](const std::unique_ptr<ActiveStream>& active) {
    if (!active->finished) {
      return false;
    }
    if (active->listener != nullptr) {
      spa_hook_remove(active->listener);
      delete active->listener;
      active->listener = nullptr;
    }
    if (active->stream != nullptr) {
      pw_stream_destroy(active->stream);
      active->stream = nullptr;
    }
    return true;
  });
}
