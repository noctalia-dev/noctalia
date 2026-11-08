#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class IpcService;
class SystemBus;
class CompositorPlatform;
struct BrightnessConfig;
struct wl_output;

struct BrightnessDisplay {
  std::string id;           // stable display id (usually connector name)
  std::string label;        // human-readable (connector name or description)
  float brightness = 0;     // 0.0–1.0 normalized
  bool controllable = true; // false when listed for display info only
  std::int32_t physicalWidth = 0;
  std::int32_t physicalHeight = 0;
  std::int32_t logicalWidth = 0;
  std::int32_t logicalHeight = 0;
  std::int32_t scale = 1;

  bool operator==(const BrightnessDisplay&) const = default;
};

// Backoff between ddcutil discovery attempts while a configured display has no control; discovery
// gives up after the last delay. Tests shorten it to avoid waiting on real time.
struct BrightnessDdcRetryDelays {
  std::array<std::chrono::milliseconds, 3> delays{
      std::chrono::seconds{1}, std::chrono::seconds{2}, std::chrono::seconds{4}
  };
};

class BrightnessService {
public:
  using ChangeCallback = std::function<void()>;
  enum class BatchChangePhase {
    Begin,
    End,
  };

  BrightnessService(
      SystemBus* bus, CompositorPlatform& platform, const BrightnessConfig& config,
      BrightnessDdcRetryDelays retryDelays = {}
  );
  ~BrightnessService();

  BrightnessService(const BrightnessService&) = delete;
  BrightnessService& operator=(const BrightnessService&) = delete;

  const std::vector<BrightnessDisplay>& displays() const noexcept;
  const BrightnessDisplay* findDisplay(const std::string& id) const;
  const BrightnessDisplay* findByOutput(wl_output* output) const;
  bool available() const noexcept;

  void setAllBrightness(float value);
  void setBrightness(const std::string& displayId, float value);
  void requestDdcRefresh();
  void requestDdcRescan();
  void reload(const BrightnessConfig& config);
  void onOutputsChanged();
  void registerIpc(IpcService& ipc, const std::function<void(BatchChangePhase)>& onBatchChange = {});

  void setChangeCallback(ChangeCallback callback);

  // Poll integration — inotify fd for external brightness changes.
  int watchFd() const noexcept;
  void dispatchWatch();

private:
  struct Impl;
  Impl* m_impl;
};
