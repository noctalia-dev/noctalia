#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <pipewire/pipewire.h>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct pw_stream;
struct spa_source;

class SoundPlayer {
public:
  static constexpr std::string_view kEventVolumeChange = "audio-volume-change";
  static constexpr std::string_view kEventNotification = "message-new-instant";
  static constexpr std::string_view kEventPowerPlug = "power-plug";
  static constexpr std::string_view kEventPowerUnplug = "power-unplug";
  static constexpr std::string_view kEventScreenCapture = "screen-capture";

  explicit SoundPlayer(pw_loop* loop);
  ~SoundPlayer();

  [[nodiscard]] static std::vector<std::pair<std::string, std::string>> availableThemes();

  SoundPlayer(const SoundPlayer&) = delete;
  SoundPlayer& operator=(const SoundPlayer&) = delete;

  void play(std::string_view name);
  void setTheme(std::string theme);
  void setShellSoundsEnabled(bool enabled);
  void setEventEnabled(std::string_view event, bool enabled);
  void setVolume(float volume);

  [[nodiscard]] std::optional<std::string>
  loadPluginSound(std::uint64_t ownerId, const std::string& name, const std::filesystem::path& path);
  void playPluginSound(std::uint64_t ownerId, const std::string& name);
  void unloadPluginSounds(std::uint64_t ownerId);

  static void onProcess(void* userdata);
  static void onStreamStateChanged(void* userdata, pw_stream_state oldState, pw_stream_state state, const char* error);
  static void onDrained(void* userdata);
  static void onStreamCloseTimer(void* userdata, std::uint64_t expirations);

private:
  struct SoundBuffer {
    std::vector<float> samples;
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
  };

  struct ActiveStream {
    SoundPlayer* owner = nullptr;
    pw_stream* stream = nullptr;
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
  spa_source* m_streamCloseTimer = nullptr;
  float m_volume = 1.0F;
  std::string m_theme;
  bool m_shellSoundsEnabled = true;
  std::set<std::string, std::less<>> m_disabledEvents;
  std::unordered_map<std::string, std::shared_ptr<const SoundBuffer>> m_buffers;
  std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::shared_ptr<const SoundBuffer>>>
      m_pluginBuffers;
  std::vector<std::unique_ptr<ActiveStream>> m_active;
};
