#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "system/surface_display_sensors.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

class BrightnessOsd;
class BrightnessService;
class CompositorPlatform;

/// Drives Surface ALS auto-brightness / accel auto-rotate via a repeating timer
/// (not a PollSource — voting timeout=0 from a poll source busy-spins the main loop).
/// Auto-rotate, Type Cover awareness, and OSK toggles are Niri-only.
class SurfaceDisplayService {
public:
  SurfaceDisplayService(
      CompositorPlatform& platform, BrightnessService* brightness, BrightnessOsd* brightnessOsd, SurfaceConfig config,
      float brightnessMinimum, bool surfaceHost
  );
  ~SurfaceDisplayService();

  SurfaceDisplayService(const SurfaceDisplayService&) = delete;
  SurfaceDisplayService& operator=(const SurfaceDisplayService&) = delete;

  void reload(const SurfaceConfig& config, float brightnessMinimum, bool surfaceHost);

private:
  [[nodiscard]] bool featuresActive() const noexcept;
  [[nodiscard]] bool niriDisplayFeaturesActive() const noexcept;
  void syncTimer();
  void tick();
  void applyAutoBrightness();
  void startBrightnessRamp(float from, float to);
  void brightnessRampTick();
  void applyAutoRotate();
  void applySlateAsTablet();
  void syncCoverAndOsk();
  void disarmOsk(std::string_view reason);
  [[nodiscard]] bool shouldAutoRotate() const;
  void setOutputTransform(std::string_view transform);

  CompositorPlatform& m_platform;
  BrightnessService* m_brightness = nullptr;
  BrightnessOsd* m_brightnessOsd = nullptr;
  SurfaceConfig m_config;
  float m_brightnessMinimum = 0.0F;
  bool m_surfaceHost = false;

  Timer m_timer;
  Timer m_brightnessRampTimer;
  float m_lastAppliedBrightness = -1.0F;
  float m_settledBrightness = -1.0F;
  float m_rampFrom = 0.0F;
  float m_rampTarget = 0.0F;
  std::chrono::steady_clock::time_point m_rampStartedAt{};
  double m_lastGoodLux = 0.0;
  double m_filteredLux = 0.0;
  bool m_loggedAlsInactive = false;
  bool m_alsInactive = false;
  bool m_loggedSlateWriteFailure = false;
  std::int32_t m_consecutiveZeroLux = 0;
  bool m_hasAppliedRotation = false;
  noctalia::system::surface::ScreenRotation m_pendingRotation =
      noctalia::system::surface::ScreenRotation::Flat;
  noctalia::system::surface::ScreenRotation m_appliedRotation =
      noctalia::system::surface::ScreenRotation::Flat;
  std::int32_t m_stableCount = 0;
  std::chrono::steady_clock::time_point m_nextRotateAllowed{};
  std::chrono::steady_clock::time_point m_lastBrightnessSample{};
  std::chrono::steady_clock::time_point m_lastCoverSample{};
  std::optional<bool> m_coverAttached;
  bool m_oskArmed = false;
};
