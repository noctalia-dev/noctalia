#include "system/gamma_service.h"

#include "core/log.h"
#include "ipc/ipc_service.h"
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

  // Schedule-follow cadence. Gamma uploads are not instant on every compositor (niri/smithay are
  // slow and hitch a frame per upload), so while following a sunset/sunrise ramp we re-evaluate at
  // this interval and push only when the rounded Kelvin actually changes — one upload per tick caps
  // the rate at 5/s. Discrete toggles (enable/disable/force/reload) snap in a single upload rather
  // than bursting many, so they do not stutter.
  constexpr int kTickIntervalMs = 200;
  // Clock-anchored sunset/sunrise ramp window. The displayed temperature is a function of how far
  // into this window the wall clock is, so it does not depend on when the app started.
  constexpr float kRampDurationMs = 1200000.0f; // 20 min
  constexpr auto kScheduleRecheckInterval = std::chrono::minutes(1);

  const zwlr_gamma_control_v1_listener kGammaControlListener = {
      .gamma_size = &GammaService::onGammaSize,
      .failed = &GammaService::onGammaFailed,
  };

} // namespace

GammaService::GammaService(WaylandConnection& wayland) : m_wayland(wayland) {}

GammaService::~GammaService() { restoreAll(); }

void GammaService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void GammaService::setStateFeedbackCallback(StateFeedbackCallback callback) { m_stateFeedback = std::move(callback); }

void GammaService::notifyStateFeedback() {
  if (m_stateFeedback) {
    m_stateFeedback();
  }
}

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
  notifyStateFeedback();
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
  notifyStateFeedback();
}

void GammaService::toggleForceEnabled() { setForceEnabled(!forceEnabled()); }

void GammaService::clearForceOverride() {
  m_forceOverride.reset();
  apply();
  notifyStateFeedback();
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
}

// --- Schedule following ---

// Upload the instantaneous target, pushing to the compositor only when the rounded Kelvin changed.
// One upload per call caps the rate at 5/s while following a drifting schedule ramp.
void GammaService::applyTarget(int kelvin) {
  if (m_currentKelvin == kelvin) {
    return;
  }
  m_currentKelvin = kelvin;
  applyGammaToAll(m_currentKelvin);
}

void GammaService::ensureTick() {
  if (!m_transitionTimer.active()) {
    m_transitionTimer.startRepeating(std::chrono::milliseconds(kTickIntervalMs), [this]() { tickGamma(); });
  }
}

void GammaService::tickGamma() {
  const GammaTarget t = computeTarget();
  if (t.kelvin < 0) {
    restoreAll();
    return;
  }
  m_targetKelvin = t.kelvin;
  applyTarget(t.kelvin);
  if (!t.transitioning) {
    m_transitionTimer.stop();
  }
}

// --- Core state machine ---

// Instantaneous, clock-anchored target. The schedule fades day<->night across a fixed ramp window
// centered on the boundary (half before, half after), so the named time is the midpoint of the
// transition. The position is derived from wall-clock time, so the result is identical whether the
// app started before or after the boundary.
GammaService::GammaTarget GammaService::computeTarget() const {
  const int dayTemp =
      std::clamp(m_config.dayTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax);
  const int nightTemp =
      std::clamp(m_config.nightTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax);

  if (dayTemp <= nightTemp) {
    return {};
  }

  if (effectiveForce()) {
    return {.kelvin = nightTemp, .transitioning = false};
  }

  const bool manualMode = day_night_schedule::isManualMode(m_location, m_resolvedLatitude, m_resolvedLongitude);
  if (!manualMode) {
    const auto coords = day_night_schedule::resolveCoordinates(m_location, m_resolvedLatitude, m_resolvedLongitude);
    if (!coords.latitude.has_value() || !coords.longitude.has_value()) {
      if (m_locationResolving || networkLocationConfigured()) {
        kLog.debug("night light schedule waiting for location resolution");
      } else if (m_location.latitude.has_value() != m_location.longitude.has_value()) {
        kLog.warn("need both latitude and longitude for manual location");
      } else {
        kLog.warn("no schedule: enable auto-locate, set an address, or set latitude/longitude or sunset/sunrise");
      }
      return {};
    }
  }

  const auto eval = day_night_schedule::evaluate(m_location, m_resolvedLatitude, m_resolvedLongitude);
  const int currentPhaseTemp = eval.night ? nightTemp : dayTemp;
  const int otherPhaseTemp = eval.night ? dayTemp : nightTemp;
  const float fade = kRampDurationMs;
  const float half = fade / 2.0f;
  const auto since = static_cast<float>(eval.sinceBoundary.count());
  const auto until = static_cast<float>(eval.untilBoundary.count());

  int from = currentPhaseTemp;
  int to = currentPhaseTemp;
  float progress = 0.0f;
  bool transitioning = false;
  if (since < half) {
    // Second half of the transition into the current phase: previous phase -> current, progress 0.5->1.
    from = otherPhaseTemp;
    to = currentPhaseTemp;
    progress = 0.5f + since / fade;
    transitioning = true;
  } else if (until < half) {
    // First half of the transition out of the current phase: current -> next phase, progress 0->0.5.
    from = currentPhaseTemp;
    to = otherPhaseTemp;
    progress = 0.5f - until / fade;
    transitioning = true;
  }

  const int kelvin =
      static_cast<int>(std::lround(std::lerp(static_cast<float>(from), static_cast<float>(to), progress)));
  return {.kelvin = kelvin, .transitioning = transitioning};
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
    restoreAll(); // instant: releasing gamma control restores the compositor's native gamma
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }

  syncOutputs();

  const GammaTarget t = computeTarget();
  if (t.kelvin < 0) {
    restoreAll();
    if (m_changeCallback) {
      m_changeCallback();
    }
    return;
  }

  if (t.kelvin != m_targetKelvin) {
    const int dayTemp =
        std::clamp(m_config.dayTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax);
    kLog.info(
        "target {}K (day={}K night={}K force={} ramping={})", t.kelvin, dayTemp,
        std::clamp(m_config.nightTemperature, NightLightConfig::kTemperatureMin, NightLightConfig::kTemperatureMax),
        effectiveForce(), t.transitioning
    );
  }
  m_targetKelvin = t.kelvin;
  applyTarget(t.kelvin); // discrete toggles (enable/force/reload) snap in a single upload
  if (t.transitioning) {
    ensureTick(); // follow the drifting clock-anchored target while inside the ramp window
  } else {
    m_transitionTimer.stop();
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
