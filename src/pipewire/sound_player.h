#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <pipewire/pipewire.h>
#include <string>
#include <unordered_map>
#include <vector>

struct pw_stream;
struct spa_hook;

class SoundPlayer {
public:
  explicit SoundPlayer(pw_loop* loop);
  ~SoundPlayer();

  SoundPlayer(const SoundPlayer&) = delete;
  SoundPlayer& operator=(const SoundPlayer&) = delete;

  bool load(const std::string& name, const std::filesystem::path& path);
  void play(const std::string& name);
  void setVolume(float volume);

  static void onProcess(void* userdata);
  static void onStreamStateChanged(void* userdata, pw_stream_state oldState, pw_stream_state state, const char* error);
  static void onDrained(void* userdata);

private:
  struct SoundBuffer {
    std::vector<float> samples;
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
  };

  struct ActiveStream {
    SoundPlayer* owner = nullptr;
    pw_stream* stream = nullptr;
    spa_hook* listener = nullptr;
    const SoundBuffer* buffer = nullptr;
    std::size_t cursor = 0;
    bool draining = false;
    bool finished = false;
  };

  void processStream(ActiveStream& streamState);
  void markFinished(ActiveStream& streamState);
  void removeFinished();

  pw_loop* m_loop = nullptr;
  float m_volume = 1.0f;
  std::unordered_map<std::string, SoundBuffer> m_buffers;
  std::vector<std::unique_ptr<ActiveStream>> m_active;
};
