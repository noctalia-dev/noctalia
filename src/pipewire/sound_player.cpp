#include "pipewire/sound_player.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <pipewire/pipewire.h>
#include <sndfile.h>
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

std::optional<std::string> SoundPlayer::decode(const std::filesystem::path& path, SoundBuffer& out) {
  SF_INFO info{};
  SNDFILE* file = sf_open(path.string().c_str(), SFM_READ, &info);
  if (file == nullptr) {
    return "failed to open audio file: " + std::string(sf_strerror(nullptr));
  }

  if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
    sf_close(file);
    return "audio file has no samples";
  }

  out.sampleRate = static_cast<std::uint32_t>(info.samplerate);
  out.channels = static_cast<std::uint32_t>(info.channels);
  out.samples.resize(static_cast<std::size_t>(info.frames) * out.channels);

  const sf_count_t readFrames = sf_readf_float(file, out.samples.data(), info.frames);
  sf_close(file);
  if (readFrames <= 0) {
    return "audio file has no samples";
  }

  out.samples.resize(static_cast<std::size_t>(readFrames) * out.channels);
  return std::nullopt;
}

bool SoundPlayer::load(const std::string& name, const std::filesystem::path& path) {
  if (name.empty() || path.empty()) {
    return false;
  }

  SoundBuffer buffer;
  if (const auto error = decode(path, buffer)) {
    kLog.warn("failed to load sound \"{}\" from {}: {}", name, path.string(), *error);
    return false;
  }

  m_buffers[name] = std::make_shared<const SoundBuffer>(std::move(buffer));
  kLog.info("loaded sound \"{}\" from {}", name, path.string());
  return true;
}

std::optional<std::string>
SoundPlayer::loadPluginSound(std::uint64_t ownerId, const std::string& name, const std::filesystem::path& path) {
  SoundBuffer buffer;
  if (const auto error = decode(path, buffer)) {
    kLog.warn("failed to load plugin sound \"{}\" from {}: {}", name, path.string(), *error);
    return error;
  }

  m_pluginBuffers[ownerId][name] = std::make_shared<const SoundBuffer>(std::move(buffer));
  kLog.info("loaded plugin sound \"{}\" from {}", name, path.string());
  return std::nullopt;
}

void SoundPlayer::unloadPluginSounds(std::uint64_t ownerId) { m_pluginBuffers.erase(ownerId); }

void SoundPlayer::play(const std::string& name) {
  const auto it = m_buffers.find(name);
  if (it == m_buffers.end()) {
    return;
  }
  playBuffer(name, it->second);
}

void SoundPlayer::playPluginSound(std::uint64_t ownerId, const std::string& name) {
  const auto ownerIt = m_pluginBuffers.find(ownerId);
  if (ownerIt == m_pluginBuffers.end()) {
    kLog.warn("plugin sound \"{}\" is not loaded", name);
    return;
  }

  const auto soundIt = ownerIt->second.find(name);
  if (soundIt == ownerIt->second.end()) {
    kLog.warn("plugin sound \"{}\" is not loaded", name);
    return;
  }
  playBuffer(name, soundIt->second);
}

void SoundPlayer::playBuffer(const std::string& name, const std::shared_ptr<const SoundBuffer>& buffer) {
  if (m_loop == nullptr || m_volume <= 0.0f || buffer->samples.empty()) {
    return;
  }

  removeFinished();

  for (const auto& active : m_active) {
    if (!active->finished && active->buffer.get() == buffer.get()) {
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
