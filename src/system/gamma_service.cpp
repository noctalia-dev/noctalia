#include "system/gamma_service.h"

#include "core/log.h"
#include "ipc/ipc_service.h"
#include "render/animation/animation.h"
#include "system/day_night_schedule.h"
#include "wayland/wayland_connection.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("gamma");

  constexpr float kTransitionDurationMs = 1500.0f;
  constexpr int kTransitionIntervalMs = 20;

  constexpr auto kScheduleRecheckInterval = std::chrono::minutes(1);

  const zwlr_gamma_control_v1_listener kGammaControlListener = {
      .gamma_size = &GammaService::onGammaSize,
      .failed = &GammaService::onGammaFailed,
  };

} // namespace

GammaService::GammaService(WaylandConnection& wayland) : m_wayland(wayland) {}

GammaService::~GammaService() { restoreAll(); }

void GammaService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void GammaService::reload(const NightLightConfig& config, const LocationConfig& location) {
  if (config.enabled != m_config.enabled) {
    m_enabledOverride.reset();
  }
  if (config.force != m_config.force) {
    m_forceOverride.reset();
  }
  m_config = config;
  m_location = location;
  apply();
}

void GammaService::setEnabled(bool enabled) {
  m_enabledOverride = enabled;
  apply();
}

void GammaService::toggleEnabled() { setEnabled(!enabled()); }

void GammaService::setLocationResolving(bool resolving) {
  if (m_locationResolving == resolving) {
    return;
  }
  m_locationResolving = resolving;
  apply();
}

void GammaService::setResolvedCoordinates(std::optional<double> latitude, std::optional<double> longitude) {
  if (latitude.has_value() && !std::isfinite(*latitude)) {
    latitude.reset();
  }
  if (longitude.has_value() && !std::isfinite(*longitude)) {
    longitude.reset();
  }
  if (m_resolvedLatitude == latitude && m_resolvedLongitude == longitude) {
    return;
  }
  m_resolvedLatitude = latitude;
  m_resolvedLongitude = longitude;
  apply();
}

void GammaService::setForceEnabled(bool enabled) {
  m_forceOverride = enabled;
  apply();
}

void GammaService::toggleForceEnabled() { setForceEnabled(!forceEnabled()); }

void GammaService::clearForceOverride() {
  m_forceOverride.reset();
  apply();
}

bool GammaService::enabled() const { return effectiveConfiguredEnabled(); }

bool GammaService::forceEnabled() const { return effectiveForce(); }

bool GammaService::active() const {
  if (!effectiveEnabled()) {
    return false;
  }
  if (effectiveForce()) {
    return true;
  }
  return isNightPhase();
}

void GammaService::onOutputsChanged() {
  if (!effectiveEnabled()) {
    return;
  }
  apply();
}

void GammaService::reevaluateSchedule() { apply(); }

bool GammaService::effectiveConfiguredEnabled() const {
  if (m_enabledOverride.has_value()) {
    return *m_enabledOverride;
  }
  return m_config.enabled;
}

bool GammaService::effectiveEnabled() const { return effectiveConfiguredEnabled() || m_forceOverride.value_or(false); }

bool GammaService::effectiveForce() const {
  if (m_forceOverride.has_value()) {
    return *m_forceOverride;
  }
  return m_config.force;
}

bool GammaService::networkLocationConfigured() const { return m_location.autoLocate || !m_location.address.empty(); }

void GammaService::scheduleManualTimer() {
  const auto boundaryDelay =
      day_night_schedule::evaluate(m_location, m_resolvedLatitude, m_resolvedLongitude).untilBoundary;
  const auto delay =
      std::min(boundaryDelay, std::chrono::duration_cast<std::chrono::milliseconds>(kScheduleRecheckInterval));
  kLog.debug(
      "manual schedule: next phase boundary in {}s, recheck in {}s", boundaryDelay.count() / 1000, delay.count() / 1000
  );
  m_scheduleTimer.start(delay, [this, boundaryTimer = delay == boundaryDelay]() {
    if (boundaryTimer) {
      kLog.info("manual schedule: phase boundary reached");
    }
    apply();
  });
}

void GammaService::scheduleGeoTimer() {
  const auto boundaryDelay =
      day_night_schedule::evaluate(m_location, m_resolvedLatitude, m_resolvedLongitude).untilBoundary;
  const auto delay =
      std::min(boundaryDelay, std::chrono::duration_cast<std::chrono::milliseconds>(kScheduleRecheckInterval));
  kLog.debug(
      "geo schedule: next phase boundary in {}s, recheck in {}s", boundaryDelay.count() / 1000, delay.count() / 1000
  );
  m_scheduleTimer.start(delay, [this, boundaryTimer = delay == boundaryDelay]() {
    if (boundaryTimer) {
      kLog.info("geo schedule: phase boundary reached");
    }
    apply();
  });
}

bool GammaService::isNightPhase() const {
  return day_night_schedule::evaluate(m_location, m_resolvedLatitude, m_resolvedLongitude).night;
}

// --- Gamma ramp math ---

GammaService::RgbMultipliers GammaService::kelvinToRgb(int kelvin) {
  const double temp = std::clamp(kelvin, 1000, 10000) / 100.0;
  RgbMultipliers mul;

  if (temp <= 66.0) {
    mul.r = 1.0;
    mul.g = std::clamp((99.4708025861 * std::log(temp) - 161.1195681661) / 255.0, 0.0, 1.0);
    if (temp <= 19.0) {
      mul.b = 0.0;
    } else {
      mul.b = std::clamp((138.5177312231 * std::log(temp - 10.0) - 305.0447927307) / 255.0, 0.0, 1.0);
    }
  } else {
    mul.r = std::clamp(329.698727446 * std::pow(temp - 60.0, -0.1332047592) / 255.0, 0.0, 1.0);
    mul.g = std::clamp(288.1221695283 * std::pow(temp - 60.0, -0.0755148492) / 255.0, 0.0, 1.0);
    mul.b = 1.0;
  }

  return mul;
}

void GammaService::fillGammaRamp(std::uint16_t* ramp, std::uint32_t size, const RgbMultipliers& mul) {
  const double scale = 65535.0 / static_cast<double>(size - 1);
  for (std::uint32_t i = 0; i < size; ++i) {
    const double base = i * scale;
    ramp[i] = static_cast<std::uint16_t>(std::clamp(mul.r * base, 0.0, 65535.0));
    ramp[size + i] = static_cast<std::uint16_t>(std::clamp(mul.g * base, 0.0, 65535.0));
    ramp[2 * size + i] = static_cast<std::uint16_t>(std::clamp(mul.b * base, 0.0, 65535.0));
  }
}

// --- Per-output gamma management ---

void GammaService::onGammaSize(void* data, zwlr_gamma_control_v1* /*ctrl*/, std::uint32_t size) {
  auto* og = static_cast<OutputGamma*>(data);
  og->gammaSize = size;
  og->ready = true;
  if (og->owner != nullptr && og->owner->m_currentKelvin >= 0) {
    og->owner->applyGammaToOutput(*og, og->owner->m_currentKelvin);
  }
}

void GammaService::onGammaFailed(void* data, zwlr_gamma_control_v1* /*ctrl*/) {
  auto* og = static_cast<OutputGamma*>(data);
  kLog.warn("gamma control failed for an output");
  og->ready = false;
  if (og->owner != nullptr) {
    og->owner->destroyOutputGamma(*og);
  }
}

void GammaService::syncOutputs() {
  if (!m_wayland.hasGammaControl()) {
    return;
  }

  const auto& wlOutputs = m_wayland.outputs();

  // Remove entries for outputs that no longer exist.
  std::erase_if(m_outputs, [&](OutputGamma& og) {
    for (const auto& wo : wlOutputs) {
      if (wo.output == og.wlOutput) {
        return false;
      }
    }
    destroyOutputGamma(og);
    return true;
  });

  // Add entries for new outputs.
  for (const auto& wo : wlOutputs) {
    if (wo.output == nullptr) {
      continue;
    }
    bool found = false;
    for (const auto& og : m_outputs) {
      if (og.wlOutput == wo.output) {
        found = true;
        break;
      }
    }
    if (found) {
      continue;
    }

    auto* ctrl = zwlr_gamma_control_manager_v1_get_gamma_control(m_wayland.gammaControlManager(), wo.output);
    auto& og = m_outputs.emplace_back(
        OutputGamma{
            .wlOutput = wo.output,
            .control = ctrl,
            .gammaSize = 0,
            .ready = false,
            .owner = this,
        }
    );
    zwlr_gamma_control_v1_add_listener(ctrl, &kGammaControlListener, &og);
  }
}

void GammaService::destroyOutputGamma(OutputGamma& og) {
  if (og.control != nullptr) {
    zwlr_gamma_control_v1_destroy(og.control);
    og.control = nullptr;
  }
  og.ready = false;
  og.gammaSize = 0;
}

void GammaService::applyGammaToOutput(OutputGamma& og, int kelvin) {
  if (og.control == nullptr || og.gammaSize == 0 || !og.ready) {
    return;
  }

  const std::size_t tableBytes = 3 * og.gammaSize * sizeof(std::uint16_t);
  const int fd = memfd_create("gamma", MFD_CLOEXEC);
  if (fd < 0) {
    kLog.warn("memfd_create failed");
    return;
  }

  if (ftruncate(fd, static_cast<off_t>(tableBytes)) < 0) {
    ::close(fd);
    kLog.warn("ftruncate failed for gamma ramp");
    return;
  }

  auto* data = static_cast<std::uint16_t*>(mmap(nullptr, tableBytes, PROT_WRITE, MAP_SHARED, fd, 0));
  if (data == MAP_FAILED) {
    ::close(fd);
    kLog.warn("mmap failed for gamma ramp");
    return;
  }

  const auto mul = kelvinToRgb(kelvin);
  fillGammaRamp(data, og.gammaSize, mul);
  munmap(data, tableBytes);

  zwlr_gamma_control_v1_set_gamma(og.control, fd);
  ::close(fd);
}

void GammaService::applyGammaToAll(int kelvin) {
  for (auto& og : m_outputs) {
    applyGammaToOutput(og, kelvin);
  }
}

void GammaService::restoreAll() {
  m_transitionTimer.stop();
  for (auto& og : m_outputs) {
    destroyOutputGamma(og);
  }
  m_outputs.clear();
  m_currentKelvin = -1;
  m_targetKelvin = -1;
  m_transitionFromKelvin = -1;
  m_transitionProgress = 0.0f;
}

// --- Smooth transitions ---

void GammaService::startTransition(int fromKelvin, int toKelvin) {
  if (fromKelvin < 0) {
    const int dayTemp =
        std::clamp(m_config.dayTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax);
    fromKelvin = dayTemp;
    syncOutputs();
    m_currentKelvin = fromKelvin;
    applyGammaToAll(fromKelvin);
  }
  if (fromKelvin == toKelvin) {
    m_transitionTimer.stop();
    m_currentKelvin = toKelvin;
    m_targetKelvin = toKelvin;
    m_transitionFromKelvin = toKelvin;
    m_transitionProgress = 1.0f;
    if (m_restoreAfterTransition) {
      restoreAll();
      m_restoreAfterTransition = false;
    }
    return;
  }
  m_transitionFromKelvin = fromKelvin;
  m_targetKelvin = toKelvin;
  m_transitionProgress = 0.0f;
  m_transitionStart = std::chrono::steady_clock::now();
  m_transitionTimer.startRepeating(std::chrono::milliseconds(kTransitionIntervalMs), [this]() { tickTransition(); });
}

void GammaService::tickTransition() {
  const auto elapsed = std::chrono::steady_clock::now() - m_transitionStart;
  m_transitionProgress = std::min(
      1.0f, static_cast<float>(std::chrono::duration<double, std::milli>(elapsed).count()) / kTransitionDurationMs
  );
  const auto easeProgress = applyEasing(Easing::EaseInOutCubic, m_transitionProgress);
  const int interpolated = static_cast<int>(
      std::lerp(static_cast<float>(m_transitionFromKelvin), static_cast<float>(m_targetKelvin), easeProgress)
  );
  if (interpolated != m_currentKelvin) {
    applyGammaToAll(interpolated);
    m_currentKelvin = interpolated;
  }
  if (m_transitionProgress >= 1.0f) {
    m_transitionTimer.stop();
    if (m_currentKelvin != m_targetKelvin) {
      applyGammaToAll(m_targetKelvin);
    }
    m_currentKelvin = m_targetKelvin;
    if (m_restoreAfterTransition) {
      restoreAll();
      m_restoreAfterTransition = false;
    }
  }
}

void GammaService::stopTransition() {
  m_transitionTimer.stop();
  if (m_targetKelvin >= 0) {
    applyGammaToAll(m_targetKelvin);
    m_currentKelvin = m_targetKelvin;
  }
}

// --- Core state machine ---

int GammaService::targetTemperature() const {
  const int dayTemp =
      std::clamp(m_config.dayTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax);
  const int nightTemp =
      std::clamp(m_config.nightTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax);

  if (dayTemp <= nightTemp) {
    return -1;
  }

  if (effectiveForce()) {
    return nightTemp;
  }

  const bool manualMode = day_night_schedule::isManualMode(m_location, m_resolvedLatitude, m_resolvedLongitude);
  if (manualMode) {
    return day_night_schedule::evaluate(m_location, m_resolvedLatitude, m_resolvedLongitude).night ? nightTemp
                                                                                                   : dayTemp;
  }

  const auto coords = day_night_schedule::resolveCoordinates(m_location, m_resolvedLatitude, m_resolvedLongitude);
  if (!coords.latitude.has_value() || !coords.longitude.has_value()) {
    if (m_locationResolving || networkLocationConfigured()) {
      kLog.debug("night light schedule waiting for location resolution");
    } else if (m_location.latitude.has_value() != m_location.longitude.has_value()) {
      kLog.warn("need both latitude and longitude for manual location");
    } else {
      kLog.warn("no schedule: enable auto-locate, set an address, or set latitude/longitude or sunset/sunrise");
    }
    return -1;
  }

  return day_night_schedule::evaluate(m_location, m_resolvedLatitude, m_resolvedLongitude).night ? nightTemp : dayTemp;
}

void GammaService::apply() {
  if (!m_wayland.hasGammaControl()) {
    if (!m_gammaUnavailableLogged) {
      kLog.warn("compositor does not support gamma control");
      m_gammaUnavailableLogged = true;
    }
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }

  const bool manualMode = day_night_schedule::isManualMode(m_location, m_resolvedLatitude, m_resolvedLongitude);
  if (effectiveEnabled() && manualMode) {
    scheduleManualTimer();
  } else if (effectiveEnabled() && !effectiveForce()) {
    const auto coords = day_night_schedule::resolveCoordinates(m_location, m_resolvedLatitude, m_resolvedLongitude);
    if (coords.latitude.has_value() && coords.longitude.has_value()) {
      scheduleGeoTimer();
    } else {
      m_scheduleTimer.stop();
    }
  } else {
    m_scheduleTimer.stop();
  }

  if (!effectiveEnabled()) {
    m_scheduleTimer.stop();
    if (m_currentKelvin > 0) {
      const int dayTemp =
          std::clamp(m_config.dayTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax);
      m_restoreAfterTransition = true;
      startTransition(m_currentKelvin, dayTemp);
    } else {
      restoreAll();
    }
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }

  m_restoreAfterTransition = false;

  syncOutputs();

  const int target = targetTemperature();
  if (target < 0) {
    stopTransition();
    restoreAll();
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }

  if (target != m_targetKelvin) {
    const int dayTemp =
        std::clamp(m_config.dayTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax);
    kLog.info(
        "applying {}K (day={}K night={}K force={})", target, dayTemp,
        std::clamp(m_config.nightTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax),
        effectiveForce()
    );
    startTransition(m_currentKelvin, target);
  }

  if (m_changeCallback) {
    m_changeCallback();
  }
}

void GammaService::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "nightlight-enable",
      [this](const std::string&) -> std::string {
        setEnabled(true);
        return "ok\n";
      },
      "nightlight-enable", "Enable night light schedule"
  );

  ipc.registerHandler(
      "nightlight-disable",
      [this](const std::string&) -> std::string {
        setEnabled(false);
        return "ok\n";
      },
      "nightlight-disable", "Disable night light schedule"
  );

  ipc.registerHandler(
      "nightlight-toggle",
      [this](const std::string&) -> std::string {
        toggleEnabled();
        return "ok\n";
      },
      "nightlight-toggle", "Toggle night light schedule"
  );

  ipc.registerHandler(
      "nightlight-force-toggle",
      [this](const std::string&) -> std::string {
        toggleForceEnabled();
        return "ok\n";
      },
      "nightlight-force-toggle", "Toggle forced night light mode"
  );
}
