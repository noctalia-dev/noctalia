#pragma once

#include <chrono>
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct AudioNode;
class PipeWireService;

class PipeWireSpectrum {
public:
  using ChangeCallback = std::function<void()>;
  using ListenerId = std::uint64_t;

  explicit PipeWireSpectrum(PipeWireService& service);
  ~PipeWireSpectrum();

  PipeWireSpectrum(const PipeWireSpectrum&) = delete;
  PipeWireSpectrum& operator=(const PipeWireSpectrum&) = delete;

  ListenerId addChangeListener(int bandCount, ChangeCallback callback);
  void removeChangeListener(ListenerId id);

  void setTargetNodeId(std::uint32_t id);
  [[nodiscard]] std::uint32_t targetNodeId() const noexcept { return m_targetNodeId; }

  void setLowerCutoff(int freq);
  [[nodiscard]] int lowerCutoff() const noexcept { return m_lowerCutoff; }

  void setUpperCutoff(int freq);
  [[nodiscard]] int upperCutoff() const noexcept { return m_upperCutoff; }

  void setNoiseReduction(float amount);
  [[nodiscard]] float noiseReduction() const noexcept { return m_noiseReduction; }

  void setSmoothing(bool enabled);
  [[nodiscard]] bool smoothing() const noexcept { return m_smoothing; }

  [[nodiscard]] const std::vector<float>& values(ListenerId id) const noexcept;
  [[nodiscard]] bool idle() const noexcept { return m_idle; }

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();
  void handleAudioStateChanged();

private:
  static constexpr int kFrameRateHz = 60;

  struct ListenerState {
    int bandCount = 32;
    ChangeCallback callback;
    std::vector<int> analysisBandLow;
    std::vector<int> analysisBandHigh;
    std::vector<float> values;
    std::vector<float> workBands;
    std::vector<float> prevBands;
    std::vector<float> peak;
    std::vector<float> fall;
    std::vector<float> mem;
  };

  class Stream;
  friend class Stream;

  [[nodiscard]] bool hasListeners() const noexcept { return !m_listeners.empty(); }
  void rebuildStream();
  [[nodiscard]] std::uint32_t resolvedTargetNodeId() const noexcept;
  [[nodiscard]] const AudioNode* resolvedTargetNode() const noexcept;
  void clearValues(bool notify);
  void emitChanged(ListenerId id);

  void initProcessing();
  void reconfigureAnalysisLayout();
  void configureListenerState(ListenerState& state, bool resetState);
  void resetListenerState(ListenerState& state, bool clearValues);
  bool processListenerView(ListenerState& state, float nrFactor, double gravityMod);
  [[nodiscard]] std::chrono::nanoseconds frameInterval() const noexcept;
  void computeAnalysisBandBins();
  void feedSamples(const float* monoSamples, int count);
  void processFrame();

  PipeWireService& m_service;
  std::unordered_map<ListenerId, ListenerState> m_listeners;
  ListenerId m_nextListenerId = 1;

  std::uint32_t m_targetNodeId = 0;
  std::uint32_t m_boundNodeId = 0;
  std::string m_boundTargetObject;
  int m_analysisBandCount = 32;
  int m_lowerCutoff = 20;
  int m_upperCutoff = 20000;
  float m_noiseReduction = 0.77f;
  bool m_smoothing = true;
  std::vector<float> m_analysisBands;
  bool m_idle = true;

  std::unique_ptr<Stream> m_stream;
  std::chrono::steady_clock::time_point m_nextFrameAt;

  static constexpr int kFftSize = 4096;
  std::vector<float> m_ringBuffer;
  int m_ringPos = 0;
  bool m_ringFull = false;
  int m_sampleRate = 48000;
  int m_idleFrames = 0;
  bool m_samplesReceived = false;

  std::vector<float> m_window;
  std::vector<float> m_analysisBandBins;
  float m_sensitivity = 1.0f;
  bool m_sensInit = true;
  std::vector<std::complex<float>> m_fftBuf;
};
