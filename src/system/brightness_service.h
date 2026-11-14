#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class IpcService;
class SystemBus;
class CompositorPlatform;
struct BrightnessConfig;
struct LocationConfig;
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

struct BrightnessScheduleTiming {
  // Fade writes are paced to one brightness step (1%, or one raw unit when coarser) per write,
  // clamped to this range: the floor protects slow DDC buses, the ceiling bounds clock drift.
  std::chrono::milliseconds minTickInterval{std::chrono::seconds(2)};
  std::chrono::milliseconds maxTickInterval{std::chrono::seconds(60)};
  std::chrono::milliseconds halfFade{std::chrono::minutes(30)};
};

using BrightnessClockFn = std::function<std::chrono::system_clock::time_point()>;

class BrightnessService {
public:
  using ChangeCallback = std::function<void()>;
  enum class BatchChangePhase {
    Begin,
    End,
  };

  BrightnessService(
      SystemBus* bus, CompositorPlatform& platform, const BrightnessConfig& config, const LocationConfig& location,
      BrightnessDdcRetryDelays retryDelays = {}, BrightnessScheduleTiming timing = {}, BrightnessClockFn clockFn = {}
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
  void reload(const BrightnessConfig& config, const LocationConfig& location);
  void setResolvedCoordinates(std::optional<double> latitude, std::optional<double> longitude);
  void onOutputsChanged();
  void registerIpc(IpcService& ipc, const std::function<void(BatchChangePhase)>& onBatchChange = {});

  void setChangeCallback(ChangeCallback callback);

  bool lastChangeWasScheduled() const noexcept;

  // Poll integration — inotify fd for external brightness changes.
  int watchFd() const noexcept;
  void dispatchWatch();

private:
  struct Impl;
  Impl* m_impl;
};
