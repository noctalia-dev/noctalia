#pragma once

#include "config/config_service.h"
#include "core/timer_manager.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <string_view>

class IpcService;
class WaylandConnection;
struct wl_output;
struct zwlr_gamma_control_v1;

class GammaService {
public:
  using ChangeCallback = std::function<void()>;
  using StateFeedbackCallback = std::function<void()>;

  explicit GammaService(WaylandConnection& wayland);
  ~GammaService();

  GammaService(const GammaService&) = delete;
  GammaService& operator=(const GammaService&) = delete;

  void reload(const NightLightConfig& config, const LocationConfig& location);
  void setLocationResolving(bool resolving);
  void setResolvedCoordinates(std::optional<double> latitude, std::optional<double> longitude);
  void setEnabled(bool enabled);
  void toggleEnabled();
  void setForceEnabled(bool enabled);
  void toggleForceEnabled();
  void clearForceOverride();
  void setChangeCallback(ChangeCallback callback);
  void setStateFeedbackCallback(StateFeedbackCallback callback);
  void onOutputsChanged();
  void reevaluateSchedule();

  [[nodiscard]] bool enabled() const;
  [[nodiscard]] bool forceEnabled() const;
  [[nodiscard]] bool active() const;

  void registerIpc(IpcService& ipc);

  static void onGammaSize(void* data, zwlr_gamma_control_v1* ctrl, std::uint32_t size);
  static void onGammaFailed(void* data, zwlr_gamma_control_v1* ctrl);

private:
  [[nodiscard]] bool effectiveConfiguredEnabled() const;
  [[nodiscard]] bool effectiveEnabled() const;
  [[nodiscard]] bool effectiveForce() const;
  [[nodiscard]] bool networkLocationConfigured() const;

  void scheduleManualTimer();
  void scheduleGeoTimer();

  void apply();

  struct GammaTarget {
    int kelvin = -1;            // instantaneous clock-anchored target, -1 when no schedule applies
    bool transitioning = false; // true while inside the fade ramp window (target still drifting)
  };
  [[nodiscard]] GammaTarget computeTarget() const;
  [[nodiscard]] bool isNightPhase() const;

  struct OutputGamma {
    wl_output* wlOutput = nullptr;
    zwlr_gamma_control_v1* control = nullptr;
    std::uint32_t gammaSize = 0;
    bool ready = false;
    GammaService* owner = nullptr;
  };

  void syncOutputs();
  void destroyOutputGamma(OutputGamma& og);
  void applyGammaToOutput(OutputGamma& og, int kelvin);
  void applyGammaToAll(int kelvin);
  void restoreAll();

  void applyTarget(int kelvin);
  [[nodiscard]] std::chrono::milliseconds transitionTickInterval() const;
  void ensureTick();
  void tickGamma();

  void notifyStateFeedback();

  struct RgbMultipliers {
    double r = 1.0;
    double g = 1.0;
    double b = 1.0;
  };
  [[nodiscard]] static RgbMultipliers kelvinToRgb(int kelvin);
  static void fillGammaRamp(std::uint16_t* ramp, std::uint32_t size, const RgbMultipliers& mul);

  WaylandConnection& m_wayland;
  NightLightConfig m_config;
  LocationConfig m_location;
  std::optional<bool> m_enabledOverride;
  std::optional<bool> m_forceOverride;
  bool m_locationResolving = false;
  std::optional<double> m_resolvedLatitude;
  std::optional<double> m_resolvedLongitude;
  ChangeCallback m_changeCallback;
  StateFeedbackCallback m_stateFeedback;

  std::list<OutputGamma> m_outputs;

  int m_currentKelvin = -1;
  int m_targetKelvin = -1;
  int m_lastProfiledKelvin = -1; // probe only: previous upload's Kelvin, to report the step size
  Timer m_transitionTimer;

  Timer m_scheduleTimer;
  bool m_gammaUnavailableLogged = false;
};
