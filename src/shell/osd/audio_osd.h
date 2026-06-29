#pragma once

#include <chrono>
#include <cstdint>

class OsdOverlay;
class PipeWireService;

class AudioOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void setSoundPlayer(class SoundPlayer* soundPlayer);
  void primeFromService(const PipeWireService& service);
  void suppressFor(std::chrono::milliseconds duration);
  void showOutput(std::uint32_t sinkId, float volume, bool muted, bool playFeedback = true);
  void showInput(std::uint32_t sourceId, float volume, bool muted, bool playFeedback = true);
  void onAudioStateChanged(const PipeWireService& service);

private:
  OsdOverlay* m_overlay = nullptr;
  std::uint32_t m_lastSinkId = 0;
  float m_lastSinkVolume = -1.0f;
  int m_lastSinkPercent = -1;
  bool m_lastSinkMuted = false;
  std::uint32_t m_lastSourceId = 0;
  float m_lastSourceVolume = -1.0f;
  int m_lastSourcePercent = -1;
  bool m_lastSourceMuted = false;
  std::chrono::steady_clock::time_point m_suppressUntil;
  std::chrono::steady_clock::time_point m_suppressAutoInputOsdUntil;
  std::chrono::steady_clock::time_point m_lastSoundAt;
  class SoundPlayer* m_soundPlayer = nullptr;
};
