#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

  [[nodiscard]] std::optional<std::string>
  loadPluginSound(std::uint64_t ownerId, const std::string& name, const std::filesystem::path& path);
  void playPluginSound(std::uint64_t ownerId, const std::string& name);
  void unloadPluginSounds(std::uint64_t ownerId);

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
    std::shared_ptr<const SoundBuffer> buffer;
    std::size_t cursor = 0;
    bool draining = false;
    bool finished = false;
  };

  [[nodiscard]] static std::optional<std::string> decode(const std::filesystem::path& path, SoundBuffer& out);
  void playBuffer(const std::string& name, const std::shared_ptr<const SoundBuffer>& buffer);

  void processStream(ActiveStream& streamState);
  void markFinished(ActiveStream& streamState);
  void removeFinished();

  pw_loop* m_loop = nullptr;
  float m_volume = 1.0F;
  std::unordered_map<std::string, std::shared_ptr<const SoundBuffer>> m_buffers;
  std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::shared_ptr<const SoundBuffer>>>
      m_pluginBuffers;
  std::vector<std::unique_ptr<ActiveStream>> m_active;
};
