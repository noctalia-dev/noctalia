#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <spa/param/param.h>
#include <string>
#include <unordered_map>
#include <vector>

struct pw_context;
struct pw_core;
struct pw_client;
struct pw_device;
struct pw_loop;
struct pw_registry;
struct spa_hook;
struct spa_dict;

class ConfigService;
class IpcService;

struct AudioNode {
  std::uint32_t id = 0;
  std::string name;
  std::string description;
  std::string applicationName;
  std::string applicationId;
  std::string applicationBinary;
  std::string streamTitle;
  std::string iconName;
  std::string mediaClass; // "Audio/Sink", "Audio/Source"
  float volume = 1.0f;
  bool muted = false;
  std::uint32_t channelCount = 0;
  bool isDefault = false;

  bool operator==(const AudioNode&) const = default;
};

struct AudioState {
  std::vector<AudioNode> sinks;
  std::vector<AudioNode> sources;
  std::vector<AudioNode> programOutputs; // "Stream/Output/Audio" (application playback streams)
  std::uint32_t defaultSinkId = 0;
  std::uint32_t defaultSourceId = 0;

  bool operator==(const AudioState&) const = default;
};

class PipeWireService {
public:
  using ChangeCallback = std::function<void()>;
  using VolumePreviewCallback = std::function<void(bool isInput, std::uint32_t id, float volume, bool muted)>;

  PipeWireService();
  ~PipeWireService();

  PipeWireService(const PipeWireService&) = delete;
  PipeWireService& operator=(const PipeWireService&) = delete;

  void setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }
  void setVolumePreviewCallback(VolumePreviewCallback callback) { m_volumePreviewCallback = std::move(callback); }

  // Poll integration
  [[nodiscard]] int fd() const noexcept;
  void dispatch();
  [[nodiscard]] pw_core* coreHandle() const noexcept { return m_core; }
  [[nodiscard]] pw_loop* loop() const noexcept { return m_loop; }

  // State
  [[nodiscard]] const AudioState& state() const noexcept { return m_state; }
  [[nodiscard]] const AudioNode* defaultSink() const noexcept;
  [[nodiscard]] const AudioNode* defaultSource() const noexcept;

  // Control
  void setSinkVolume(std::uint32_t id, float volume);
  void setSinkMuted(std::uint32_t id, bool muted);
  void setDefaultSink(std::uint32_t id);
  void setSourceVolume(std::uint32_t id, float volume);
  void setSourceMuted(std::uint32_t id, bool muted);
  void setDefaultSource(std::uint32_t id);

  // Convenience (operates on default sink/source)
  void setVolume(float volume);
  void setMuted(bool muted);
  void setMicVolume(float volume);
  void setMicMuted(bool muted);
  void emitVolumePreview(bool isInput, std::uint32_t id, float volume) const;

  // Program/application streams (PipeWire "Stream/*/Audio")
  void setProgramOutputVolume(std::uint32_t id, float volume);
  void setProgramOutputMuted(std::uint32_t id, bool muted);

  // Registers audio-related IPC commands (set/raise/lower-volume, mute, set/raise/lower-mic-volume, mute-mic).
  void registerIpc(IpcService& ipc, const ConfigService& config);

  [[nodiscard]] std::uint64_t changeSerial() const noexcept { return m_changeSerial; }

  // Called from C callbacks in the .cpp - must be public
  struct DeviceRouteData {
    std::int32_t index = -1;
    std::int32_t device = -1;
    std::uint32_t direction = 0;
    std::int32_t priority = 0;
    std::uint32_t available = SPA_PARAM_AVAILABILITY_unknown;
    bool muted = false;
  };
  struct NodeData {
    PipeWireService* service = nullptr;
    std::uint32_t id = 0;
    std::uint32_t clientId = 0;
    std::string name;
    std::string description;
    std::string applicationName;
    std::string applicationId;
    std::string applicationBinary;
    std::string streamTitle;
    std::string iconName;
    std::string mediaClass;
    float volume = 1.0f;
    // Software / node-route mute from PipeWire props (SPA_PARAM_Props, node routes).
    bool swMute = false;
    bool nodeRouteMute = false;
    // Effective mute for UI (includes device-route mute, e.g. USB mic hardware switch).
    bool muted = false;
    std::uint32_t channelCount = 0;
    std::uint32_t deviceId = 0;
    bool hasRoute = false;
    std::int32_t routeIndex = -1;
    std::int32_t routeDevice = -1;
    std::uint32_t routeDirection = 0;
    std::vector<DeviceRouteData> routes;
    struct pw_node* proxy = nullptr;
    spa_hook* listener = nullptr;
  };
  struct ClientData {
    PipeWireService* service = nullptr;
    std::uint32_t id = 0;
    std::string name;
    std::string appId;
    std::string binary;
    std::string iconName;
    struct pw_client* proxy = nullptr;
    spa_hook* listener = nullptr;
  };
  struct DeviceData {
    PipeWireService* service = nullptr;
    std::uint32_t id = 0;
    struct pw_device* proxy = nullptr;
    spa_hook* listener = nullptr;
    std::vector<DeviceRouteData> routes;
  };
  void onRegistryGlobal(std::uint32_t id, const char* type, std::uint32_t version, const struct spa_dict* props);
  void onRegistryGlobalRemove(std::uint32_t id);
  void onClientInfo(std::uint32_t id, const struct pw_client_info* info);
  void onDeviceInfo(std::uint32_t id, const struct pw_device_info* info);
  void onDeviceParam(
      std::uint32_t id, std::uint32_t paramId, std::uint32_t index, std::uint32_t next, const struct spa_pod* param
  );
  void onNodeInfo(std::uint32_t id, const struct pw_node_info* info);
  void onNodeParam(
      std::uint32_t id, std::uint32_t paramId, std::uint32_t index, std::uint32_t next, const struct spa_pod* param
  );
  void parseDefaultNodes(const struct spa_dict* props);

private:
  bool m_pendingDefaultAudioDevicePropsEnum = false;
  void enumDefaultAudioDeviceParams();

  void rebuildState();
  void refreshNodeIdentity(NodeData& nd);
  void applyVolumePropsFromDict(NodeData& nd, const spa_dict* props, bool applyMixerFieldsFromDict = true);
  void recomputeEffectiveMute(NodeData& nd);
  void setNodeVolume(std::uint32_t id, float volume);
  void setNodeMuted(std::uint32_t id, bool muted);
  void setDefaultNode(std::uint32_t id, const char* key);

  void scheduleVolumeFlush();
  void flushPendingNodeVolumes();
  [[nodiscard]] bool applyNodeVolumeImmediate(std::uint32_t id, float volume);

  Timer m_volumeThrottleTimer;
  std::unordered_map<std::uint32_t, float> m_pendingNodeVolumes;
  std::chrono::steady_clock::time_point m_lastVolumeFlushAt{};
  bool m_lastVolumeFlushValid = false;

  pw_loop* m_loop = nullptr;
  pw_context* m_context = nullptr;
  pw_core* m_core = nullptr;
  pw_registry* m_registry = nullptr;
  struct pw_metadata* m_defaultMetadata = nullptr;

  // Listener hooks (must outlive the objects they listen to)
  spa_hook* m_coreListener = nullptr;
  spa_hook* m_registryListener = nullptr;

  std::unordered_map<std::uint32_t, std::unique_ptr<NodeData>> m_nodes;
  std::unordered_map<std::uint32_t, ClientData> m_clients;
  std::unordered_map<std::uint32_t, DeviceData> m_devices;
  std::vector<std::function<void()>> m_metadataCleanups;
  std::string m_defaultSinkName;
  std::string m_defaultSourceName;
  AudioState m_state;
  ChangeCallback m_changeCallback;
  VolumePreviewCallback m_volumePreviewCallback;
  std::uint64_t m_changeSerial = 0;

  void emitChanged();
};
