#include "pipewire/pipewire_spectrum.h"

#include "core/log.h"
#include "pipewire/pipewire_service.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog{"pipewire_spectrum"};
  constexpr int kDefaultViewBandCount = 32;
  constexpr int kMaxSpectrumBands = 4096 / 2;
  constexpr float kMinSensitivity = 0.001f;
  constexpr float kMaxSensitivity = 30.0f;
  constexpr float kMaxBandLevel = 0.9f;

  int clampBandCount(int count) { return std::clamp(count, 1, kMaxSpectrumBands); }

  class BufferRequeueGuard {
  public:
    BufferRequeueGuard(pw_stream* stream, pw_buffer* buffer) : m_stream(stream), m_buffer(buffer) {}
    ~BufferRequeueGuard() {
      if (m_stream != nullptr && m_buffer != nullptr) {
        pw_stream_queue_buffer(m_stream, m_buffer);
      }
    }

  private:
    pw_stream* m_stream = nullptr;
    pw_buffer* m_buffer = nullptr;
  };

  void fft(std::complex<float>* data, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1) {
        j ^= bit;
      }
      j ^= bit;
      if (i < j) {
        std::swap(data[i], data[j]);
      }
    }

    for (int len = 2; len <= n; len <<= 1) {
      const auto angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
      const std::complex<float> wn(std::cos(angle), std::sin(angle));
      for (int i = 0; i < n; i += len) {
        std::complex<float> w(1.0f, 0.0f);
        const int half = len / 2;
        for (int j = 0; j < half; ++j) {
          const auto u = data[i + j];
          const auto v = data[i + j + half] * w;
          data[i + j] = u + v;
          data[i + j + half] = u - v;
          w *= wn;
        }
      }
    }
  }

} // namespace

const std::vector<float>& PipeWireSpectrum::values(ListenerId id) const noexcept {
  static const std::vector<float> kEmptyValues;

  const auto it = m_listeners.find(id);
  if (it == m_listeners.end()) {
    return kEmptyValues;
  }
  return it->second.values;
}

PipeWireSpectrum::ListenerId PipeWireSpectrum::addChangeListener(int bandCount, ChangeCallback callback) {
  if (!callback) {
    return 0;
  }
  const bool wasEmpty = m_listeners.empty();
  const ListenerId id = m_nextListenerId++;
  ListenerState state;
  state.bandCount = clampBandCount(bandCount);
  state.callback = std::move(callback);
  m_listeners.emplace(id, std::move(state));
  reconfigureAnalysisLayout();
  if (wasEmpty) {
    rebuildStream();
  }
  return id;
}

void PipeWireSpectrum::removeChangeListener(ListenerId id) {
  if (id == 0) {
    return;
  }
  const bool removed = m_listeners.erase(id) > 0;
  if (!removed) {
    return;
  }
  if (m_listeners.empty()) {
    rebuildStream();
    return;
  }
  reconfigureAnalysisLayout();
}

class PipeWireSpectrum::Stream {
public:
  Stream(PipeWireSpectrum& spectrum, std::uint32_t nodeId, std::string targetObject)
      : m_spectrum(spectrum), m_nodeId(nodeId), m_targetObject(std::move(targetObject)) {}
  ~Stream() { destroy(); }

  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  bool start();
  void destroy();

private:
  static const pw_stream_events kEvents;

  static void onProcess(void* data);
  static void onParamChanged(void* data, std::uint32_t id, const spa_pod* param);
  static void onStateChanged(void* data, pw_stream_state oldState, pw_stream_state state, const char* error);
  static void onDestroy(void* data);

  void handleProcess();
  void handleParamChanged(std::uint32_t id, const spa_pod* param);

  PipeWireSpectrum& m_spectrum;
  std::uint32_t m_nodeId = 0;
  std::string m_targetObject;
  pw_stream* m_stream = nullptr;
  spa_hook m_listener{};
  spa_audio_info_raw m_format = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_UNKNOWN);
  bool m_formatReady = false;
};

const pw_stream_events PipeWireSpectrum::Stream::kEvents = [] {
  pw_stream_events events{};
  events.version = PW_VERSION_STREAM_EVENTS;
  events.destroy = &PipeWireSpectrum::Stream::onDestroy;
  events.state_changed = &PipeWireSpectrum::Stream::onStateChanged;
  events.param_changed = &PipeWireSpectrum::Stream::onParamChanged;
  events.process = &PipeWireSpectrum::Stream::onProcess;
  return events;
}();

bool PipeWireSpectrum::Stream::start() {
  pw_core* core = m_spectrum.m_service.coreHandle();
  if (core == nullptr || m_nodeId == 0 || m_targetObject.empty()) {
    return false;
  }

  auto* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_NAME, "Noctalia Spectrum",
      PW_KEY_APP_NAME, "Noctalia Spectrum", PW_KEY_STREAM_MONITOR, "true", PW_KEY_STREAM_CAPTURE_SINK, "true",
      PW_KEY_MEDIA_ROLE, "Music", PW_KEY_TARGET_OBJECT, m_targetObject.c_str(), PW_KEY_NODE_PASSIVE, "true", nullptr
  );
  if (props == nullptr) {
    kLog.warn("failed to create spectrum stream properties");
    return false;
  }

  m_stream = pw_stream_new(core, "noctalia-spectrum", props);
  if (m_stream == nullptr) {
    pw_properties_free(props);
    kLog.warn("failed to create spectrum stream");
    return false;
  }

  spa_zero(m_listener);
  pw_stream_add_listener(m_stream, &m_listener, &kEvents, this);

  auto buffer = std::array<std::uint8_t, 512>{};
  auto builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
  auto params = std::array<const spa_pod*, 1>{};
  auto raw = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32);
  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw);

  const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
  const int rc = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params.data(), params.size());
  if (rc < 0) {
    kLog.warn("failed to connect spectrum stream: {}", rc);
    destroy();
    return false;
  }

  return true;
}

void PipeWireSpectrum::Stream::destroy() {
  if (m_stream == nullptr) {
    return;
  }
  spa_hook_remove(&m_listener);
  pw_stream_destroy(m_stream);
  m_stream = nullptr;
  m_formatReady = false;
}

void PipeWireSpectrum::Stream::onProcess(void* data) { static_cast<Stream*>(data)->handleProcess(); }

void PipeWireSpectrum::Stream::onParamChanged(void* data, std::uint32_t id, const spa_pod* param) {
  static_cast<Stream*>(data)->handleParamChanged(id, param);
}

void PipeWireSpectrum::Stream::onStateChanged(
    void* /*data*/, pw_stream_state /*oldState*/, pw_stream_state state, const char* error
) {
  if (state == PW_STREAM_STATE_ERROR) {
    kLog.warn("spectrum stream error: {}", error != nullptr ? error : "unknown");
  }
}

void PipeWireSpectrum::Stream::onDestroy(void* data) {
  auto* self = static_cast<Stream*>(data);
  self->m_stream = nullptr;
  spa_hook_remove(&self->m_listener);
  self->m_formatReady = false;
}

void PipeWireSpectrum::Stream::handleParamChanged(std::uint32_t id, const spa_pod* param) {
  if (param == nullptr || id != SPA_PARAM_Format) {
    return;
  }

  spa_audio_info info{};
  if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) {
    return;
  }
  if (info.media_type != SPA_MEDIA_TYPE_audio || info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
    return;
  }

  auto raw = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_UNKNOWN);
  if (spa_format_audio_raw_parse(param, &raw) < 0) {
    return;
  }
  if (raw.format != SPA_AUDIO_FORMAT_F32) {
    kLog.warn("unsupported spectrum stream format {}", static_cast<int>(raw.format));
    m_formatReady = false;
    return;
  }

  m_format = raw;
  m_formatReady = raw.channels > 0;
  if (m_formatReady) {
    m_spectrum.m_sampleRate = static_cast<int>(raw.rate);
    m_spectrum.computeAnalysisBandBins();
  }
}

void PipeWireSpectrum::Stream::handleProcess() {
  if (!m_formatReady || m_stream == nullptr) {
    return;
  }

  auto* buffer = pw_stream_dequeue_buffer(m_stream);
  if (buffer == nullptr) {
    return;
  }
  BufferRequeueGuard requeue(m_stream, buffer);

  auto* spaBuffer = buffer->buffer;
  if (spaBuffer == nullptr || spaBuffer->n_datas < 1) {
    return;
  }

  auto* data = &spaBuffer->datas[0];
  if (data->data == nullptr || data->chunk == nullptr) {
    return;
  }

  const int channelCount = static_cast<int>(m_format.channels);
  if (channelCount <= 0) {
    return;
  }

  const auto* base = static_cast<const std::uint8_t*>(data->data) + data->chunk->offset;
  const auto* samples = reinterpret_cast<const float*>(base);
  const int totalSamples = static_cast<int>(data->chunk->size / sizeof(float));
  const int frameCount = totalSamples / channelCount;
  if (frameCount <= 0) {
    return;
  }

  static thread_local std::vector<float> mono;
  const auto frameSize = static_cast<std::size_t>(frameCount);
  mono.resize(frameSize);

  if (channelCount == 1) {
    std::copy(samples, samples + frameCount, mono.begin());
  } else {
    const float invChannels = 1.0f / static_cast<float>(channelCount);
    const auto channelCountSize = static_cast<std::size_t>(channelCount);
    for (std::size_t i = 0; i < frameSize; ++i) {
      float sum = 0.0f;
      for (int c = 0; c < channelCount; ++c) {
        sum += samples[i * channelCountSize + static_cast<std::size_t>(c)];
      }
      mono[i] = sum * invChannels;
    }
  }

  m_spectrum.feedSamples(mono.data(), frameCount);

  // Skip flagging sample receipt for fully-silent batches so a paused/silent sink
  // lets processFrame() short-circuit at the m_idle gate instead of running scheduled FFT work.
  bool anyNonZero = false;
  for (float sample : mono) {
    if (sample != 0.0f) {
      anyNonZero = true;
      break;
    }
  }
  if (anyNonZero) {
    m_spectrum.m_samplesReceived = true;
  }
}

PipeWireSpectrum::PipeWireSpectrum(PipeWireService& service) : m_service(service) { initProcessing(); }

PipeWireSpectrum::~PipeWireSpectrum() = default;

void PipeWireSpectrum::setTargetNodeId(std::uint32_t id) {
  if (id == m_targetNodeId) {
    return;
  }
  m_targetNodeId = id;
  rebuildStream();
}

void PipeWireSpectrum::setLowerCutoff(int freq) {
  freq = std::max(1, freq);
  if (freq == m_lowerCutoff) {
    return;
  }
  m_lowerCutoff = freq;
  computeAnalysisBandBins();
}

void PipeWireSpectrum::setUpperCutoff(int freq) {
  freq = std::max(m_lowerCutoff + 1, freq);
  if (freq == m_upperCutoff) {
    return;
  }
  m_upperCutoff = freq;
  computeAnalysisBandBins();
}

void PipeWireSpectrum::setNoiseReduction(float amount) {
  amount = std::clamp(amount, 0.0f, 1.0f);
  if (std::abs(amount - m_noiseReduction) <= 0.0001f) {
    return;
  }
  m_noiseReduction = amount;
}

void PipeWireSpectrum::setSmoothing(bool enabled) {
  if (enabled == m_smoothing) {
    return;
  }
  m_smoothing = enabled;
}

int PipeWireSpectrum::pollTimeoutMs() const {
  if (!hasListeners() || m_stream == nullptr) {
    return -1;
  }
  if (m_idle && !m_samplesReceived) {
    return -1;
  }
  const auto now = std::chrono::steady_clock::now();
  if (m_nextFrameAt <= now) {
    return 0;
  }
  return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(m_nextFrameAt - now).count());
}

void PipeWireSpectrum::tick() {
  if (!hasListeners() || m_stream == nullptr) {
    return;
  }
  if (m_idle && !m_samplesReceived) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_nextFrameAt.time_since_epoch().count() == 0 || now >= m_nextFrameAt) {
    const bool frameClockUnset = m_nextFrameAt.time_since_epoch().count() == 0;
    processFrame();

    const auto interval = frameInterval();
    if (frameClockUnset || now - m_nextFrameAt >= interval) {
      m_nextFrameAt = now + interval;
    } else {
      do {
        m_nextFrameAt += interval;
      } while (m_nextFrameAt <= now);
    }
  }
}

void PipeWireSpectrum::handleAudioStateChanged() {
  const std::uint32_t target = resolvedTargetNodeId();
  const auto* targetNode = resolvedTargetNode();
  const std::string targetObject = targetNode != nullptr ? targetNode->name : std::string{};
  if (target != m_boundNodeId
      || targetObject != m_boundTargetObject
      || (target == 0 && m_stream != nullptr)
      || (target != 0 && targetNode == nullptr)) {
    rebuildStream();
  }
}

void PipeWireSpectrum::rebuildStream() {
  m_stream.reset();
  m_boundNodeId = 0;
  m_boundTargetObject.clear();

  const std::uint32_t target = resolvedTargetNodeId();
  const auto* targetNode = resolvedTargetNode();
  if (!hasListeners() || target == 0 || targetNode == nullptr) {
    clearValues(true);
    return;
  }
  if (targetNode->name.empty()) {
    kLog.warn("spectrum target node {} has no PipeWire node name", target);
    clearValues(true);
    return;
  }

  m_stream = std::make_unique<Stream>(*this, target, targetNode->name);
  if (!m_stream->start()) {
    m_stream.reset();
    clearValues(true);
    return;
  }

  m_boundNodeId = target;
  m_boundTargetObject = targetNode->name;
  m_ringPos = 0;
  m_ringFull = false;
  m_idleFrames = 0;
  m_samplesReceived = false;
  m_sensitivity = 0.01f;
  m_sensInit = true;
  std::ranges::fill(m_analysisBands, 0.0f);
  for (auto& [id, state] : m_listeners) {
    (void)id;
    resetListenerState(state, false);
  }
  m_nextFrameAt = std::chrono::steady_clock::now();
  if (m_idle) {
    for (const auto& [id, state] : m_listeners) {
      (void)state;
      emitChanged(id);
    }
  }
}

std::uint32_t PipeWireSpectrum::resolvedTargetNodeId() const noexcept {
  if (m_targetNodeId != 0) {
    return m_targetNodeId;
  }
  return m_service.state().defaultSinkId;
}

const AudioNode* PipeWireSpectrum::resolvedTargetNode() const noexcept {
  const std::uint32_t id = resolvedTargetNodeId();
  if (id == 0) {
    return nullptr;
  }

  const auto& state = m_service.state();
  auto sink = std::ranges::find(state.sinks, id, &AudioNode::id);
  if (sink != state.sinks.end()) {
    return &*sink;
  }
  auto source = std::ranges::find(state.sources, id, &AudioNode::id);
  if (source != state.sources.end()) {
    return &*source;
  }
  return nullptr;
}

void PipeWireSpectrum::clearValues(bool notify) {
  std::vector<ListenerId> changedListeners;
  changedListeners.reserve(m_listeners.size());
  for (auto& [id, state] : m_listeners) {
    const bool hadNonZero = std::ranges::any_of(state.values, [](float value) { return value > 0.0f; });
    const bool hadValues = !state.values.empty();
    resetListenerState(state, true);
    if (notify && (hadNonZero || hadValues)) {
      changedListeners.push_back(id);
    }
  }
  std::ranges::fill(m_analysisBands, 0.0f);
  m_idleFrames = 0;
  m_idle = true;
  m_samplesReceived = false;
  m_ringFull = false;
  if (notify) {
    for (ListenerId id : changedListeners) {
      emitChanged(id);
    }
  }
}

void PipeWireSpectrum::emitChanged(ListenerId id) {
  const auto it = m_listeners.find(id);
  if (it != m_listeners.end() && it->second.callback) {
    it->second.callback();
  }
}

void PipeWireSpectrum::initProcessing() {
  m_ringBuffer.assign(kFftSize, 0.0f);
  m_ringPos = 0;
  m_ringFull = false;
  m_fftBuf.resize(kFftSize);

  m_window.resize(kFftSize);
  for (std::size_t i = 0; i < m_window.size(); ++i) {
    m_window[i] = 0.5f
        * (1.0f
           - std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(kFftSize - 1)));
  }

  reconfigureAnalysisLayout();
}

void PipeWireSpectrum::reconfigureAnalysisLayout() {
  int maxBandCount = kDefaultViewBandCount;
  for (const auto& [id, state] : m_listeners) {
    (void)id;
    maxBandCount = std::max(maxBandCount, clampBandCount(state.bandCount));
  }

  const bool analysisChanged = maxBandCount != m_analysisBandCount;
  m_analysisBandCount = maxBandCount;
  m_analysisBands.assign(static_cast<std::size_t>(m_analysisBandCount), 0.0f);
  computeAnalysisBandBins();

  for (auto& [id, state] : m_listeners) {
    (void)id;
    configureListenerState(state, analysisChanged);
  }

  if (analysisChanged) {
    for (const auto& [id, state] : m_listeners) {
      (void)state;
      emitChanged(id);
    }
  }
}

void PipeWireSpectrum::configureListenerState(ListenerState& state, bool resetState) {
  state.bandCount = clampBandCount(state.bandCount);
  const auto bandCountSize = static_cast<std::size_t>(state.bandCount);
  state.analysisBandLow.resize(bandCountSize);
  state.analysisBandHigh.resize(bandCountSize);

  const std::int64_t analysisBandCount = std::max(1, m_analysisBandCount);
  const std::int64_t listenerBandCount = std::max(1, state.bandCount);
  for (std::size_t i = 0; i < bandCountSize; ++i) {
    const std::int64_t low = (static_cast<std::int64_t>(i) * analysisBandCount) / listenerBandCount;
    const std::int64_t high =
        ((static_cast<std::int64_t>(i + 1) * analysisBandCount) + listenerBandCount - 1) / listenerBandCount - 1;
    state.analysisBandLow[i] = std::clamp<int>(static_cast<int>(low), 0, m_analysisBandCount - 1);
    state.analysisBandHigh[i] =
        std::clamp<int>(static_cast<int>(high), state.analysisBandLow[i], m_analysisBandCount - 1);
  }

  const bool sizeMismatch = static_cast<int>(state.values.size()) != state.bandCount;
  if (resetState || sizeMismatch) {
    resetListenerState(state, true);
  }
}

void PipeWireSpectrum::resetListenerState(ListenerState& state, bool clearValues) {
  const auto bandCountSize = static_cast<std::size_t>(state.bandCount);
  state.workBands.assign(bandCountSize, 0.0f);
  state.prevBands.assign(bandCountSize, 0.0f);
  state.peak.assign(bandCountSize, 0.0f);
  state.fall.assign(bandCountSize, 0.0f);
  state.mem.assign(bandCountSize, 0.0f);
  if (clearValues || static_cast<int>(state.values.size()) != state.bandCount) {
    state.values.assign(bandCountSize, 0.0f);
  }
}

void PipeWireSpectrum::computeAnalysisBandBins() {
  const auto analysisBandCountSize = static_cast<std::size_t>(m_analysisBandCount);
  m_analysisBandBins.resize(analysisBandCountSize);

  const auto fLow = static_cast<float>(m_lowerCutoff);
  const float fHigh = static_cast<float>(std::min(m_upperCutoff, m_sampleRate / 2));
  const float ratio = fHigh / fLow;
  const int fftBins = kFftSize / 2;
  const auto sampleRate = static_cast<float>(std::max(1, m_sampleRate));
  const float denominator = static_cast<float>(std::max(1, m_analysisBandCount - 1));

  for (std::size_t i = 0; i < analysisBandCountSize; ++i) {
    const float t = static_cast<float>(i) / denominator;
    const float freq = fLow * std::pow(ratio, t);
    m_analysisBandBins[i] =
        std::clamp(freq * static_cast<float>(kFftSize) / sampleRate, 1.0f, static_cast<float>(fftBins));
  }
}

bool PipeWireSpectrum::processListenerView(ListenerState& state, float nrFactor, double gravityMod) {
  auto& bands = state.workBands;
  const auto bandCountSize = static_cast<std::size_t>(state.bandCount);
  for (std::size_t i = 0; i < bandCountSize; ++i) {
    float maxBand = 0.0f;
    for (int band = state.analysisBandLow[i]; band <= state.analysisBandHigh[i]; ++band) {
      maxBand = std::max(maxBand, m_analysisBands[static_cast<std::size_t>(band)]);
    }
    bands[i] = maxBand;
  }

  for (std::size_t i = 0; i < bandCountSize; ++i) {
    if (bands[i] < state.prevBands[i] && m_noiseReduction > 0.1f) {
      bands[i] = static_cast<float>(
          static_cast<double>(state.peak[i])
          * (1.0 - static_cast<double>(state.fall[i]) * static_cast<double>(state.fall[i]) * gravityMod)
      );
      bands[i] = std::max(bands[i], 0.0f);
      state.fall[i] += 0.04f;
    } else {
      state.peak[i] = bands[i];
      state.fall[i] = 0.0f;
    }
    state.prevBands[i] = bands[i];

    bands[i] = std::clamp(state.mem[i] * nrFactor + bands[i] * (1.0f - nrFactor), 0.0f, kMaxBandLevel);
    state.mem[i] = bands[i];
  }

  if (m_smoothing) {
    constexpr float kMonstercatFactor = 1.5f;
    constexpr float kMinSpread = 0.01f;
    for (std::size_t z = 0; z < bandCountSize; ++z) {
      float spread = bands[z] / kMonstercatFactor;
      for (std::size_t m = z; m > 0 && spread > kMinSpread;) {
        --m;
        bands[m] = std::max(bands[m], spread);
        spread /= kMonstercatFactor;
      }
      spread = bands[z] / kMonstercatFactor;
      for (std::size_t m = z + 1; m < bandCountSize && spread > kMinSpread; ++m) {
        bands[m] = std::max(bands[m], spread);
        spread /= kMonstercatFactor;
      }
    }
  }

  bool changed = false;
  for (std::size_t i = 0; i < bandCountSize; ++i) {
    const float clamped = std::clamp(bands[i], 0.0f, kMaxBandLevel);
    if (state.values[i] != clamped) {
      state.values[i] = clamped;
      changed = true;
    }
  }
  return changed;
}

std::chrono::nanoseconds PipeWireSpectrum::frameInterval() const noexcept {
  return std::chrono::nanoseconds{std::chrono::seconds{1}} / kFrameRateHz;
}

void PipeWireSpectrum::feedSamples(const float* monoSamples, int count) {
  const bool wasFull = m_ringFull;
  for (int i = 0; i < count; ++i) {
    m_ringBuffer[static_cast<std::size_t>(m_ringPos)] = monoSamples[i];
    m_ringPos = (m_ringPos + 1) % kFftSize;
    if (m_ringPos == 0) {
      m_ringFull = true;
    }
  }
  if (!wasFull && m_ringFull) {
    kLog.debug("spectrum ring buffer primed");
  }
}

void PipeWireSpectrum::processFrame() {
  if (!m_ringFull) {
    m_samplesReceived = false;
    return;
  }
  if (m_idle && !m_samplesReceived) {
    return;
  }

  if (!m_samplesReceived) {
    for (auto& sample : m_ringBuffer) {
      sample *= 0.85f;
    }
  }
  m_samplesReceived = false;

  for (std::size_t i = 0; i < m_fftBuf.size(); ++i) {
    const auto idx = (static_cast<std::size_t>(m_ringPos) + i) % static_cast<std::size_t>(kFftSize);
    m_fftBuf[i] = {m_ringBuffer[idx] * m_window[i], 0.0f};
  }

  fft(m_fftBuf.data(), kFftSize);

  auto& bands = m_analysisBands;
  const auto analysisBandCountSize = static_cast<std::size_t>(m_analysisBandCount);
  for (std::size_t i = 0; i < analysisBandCountSize; ++i) {
    const float sampleBin = i < m_analysisBandBins.size() ? m_analysisBandBins[i] : 1.0f;
    const int binLow = std::clamp(static_cast<int>(std::floor(sampleBin)), 1, kFftSize / 2);
    const int binHigh = std::clamp(binLow + 1, binLow, kFftSize / 2);
    const float t = std::clamp(sampleBin - static_cast<float>(binLow), 0.0f, 1.0f);
    const float low = std::abs(m_fftBuf[static_cast<std::size_t>(binLow)]);
    const float high = std::abs(m_fftBuf[static_cast<std::size_t>(binHigh)]);
    bands[i] = low + (high - low) * t;
  }

  const float nrFactor = m_noiseReduction;
  const float noiseGate = nrFactor * static_cast<float>(kFftSize) * 0.00005f;
  constexpr float kMagnitudeCompression = 0.15f;
  for (auto& band : bands) {
    band = std::max(0.0f, band - noiseGate);
    // Log compression keeps quiet treble visible next to loud bass: a large linear
    // magnitude ratio collapses to a small additive offset, so one band can't crush the rest.
    band = std::log1p(band * kMagnitudeCompression) / kMagnitudeCompression;
    band *= m_sensitivity;
  }

  const double gravityMod = std::max(1.0, 1.54 / std::max(static_cast<double>(m_noiseReduction), 0.01));

  bool overshoot = false;
  bool silence = true;

  for (std::size_t i = 0; i < analysisBandCountSize; ++i) {
    if (bands[i] > kMaxBandLevel) {
      overshoot = true;
      bands[i] = kMaxBandLevel;
    }
    if (bands[i] > 0.01f) {
      silence = false;
    }
  }

  if (overshoot) {
    m_sensitivity *= 0.98f;
    m_sensInit = false;
  } else if (!silence) {
    m_sensitivity *= 1.001f;
    if (m_sensInit) {
      m_sensitivity *= 1.1f;
    }
  }
  m_sensitivity = std::clamp(m_sensitivity, kMinSensitivity, kMaxSensitivity);

  for (auto& band : bands) {
    band = std::clamp(band, 0.0f, kMaxBandLevel);
  }

  if (silence) {
    ++m_idleFrames;
    if (m_idleFrames >= kFrameRateHz) {
      if (!m_idle) {
        m_idle = true;
        clearValues(true);
      }
      return;
    }
  } else {
    m_idleFrames = 0;
    m_idle = false;
  }

  std::vector<ListenerId> changedListeners;
  changedListeners.reserve(m_listeners.size());
  for (auto& [id, state] : m_listeners) {
    if (processListenerView(state, nrFactor, gravityMod)) {
      changedListeners.push_back(id);
    }
  }

  for (ListenerId id : changedListeners) {
    emitChanged(id);
  }
}
