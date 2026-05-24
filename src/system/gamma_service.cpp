#include "system/gamma_service.h"

#include "core/log.h"
#include "ipc/ipc_service.h"
#include "wayland/wayland_connection.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <numbers>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("gamma");

  constexpr float kTransitionDurationMs = 1500.0f;
  constexpr int kTransitionIntervalMs = 100;
  constexpr auto kScheduleRecheckInterval = std::chrono::minutes(1);

  int timeToMinutes(std::string_view hhmm) {
    return (hhmm[0] - '0') * 600 + (hhmm[1] - '0') * 60 + (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
  }

  const zwlr_gamma_control_v1_listener kGammaControlListener = {
      .gamma_size = &GammaService::onGammaSize,
      .failed = &GammaService::onGammaFailed,
  };

} // namespace

GammaService::GammaService(WaylandConnection& wayland) : m_wayland(wayland) {}

GammaService::~GammaService() { restoreAll(); }

void GammaService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void GammaService::reload(const NightLightConfig& config) {
  if (config.enabled != m_config.enabled) {
    m_enabledOverride.reset();
  }
  if (config.force != m_config.force) {
    m_forceOverride.reset();
  }
  m_config = config;
  apply();
}

void GammaService::setEnabled(bool enabled) {
  m_enabledOverride = enabled;
  apply();
}

void GammaService::toggleEnabled() { setEnabled(!enabled()); }

void GammaService::setWeatherLocationConfigured(bool configured) {
  if (m_weatherLocationConfigured == configured) {
    return;
  }
  m_weatherLocationConfigured = configured;
  apply();
}

void GammaService::setWeatherCoordinates(std::optional<double> latitude, std::optional<double> longitude) {
  if (latitude.has_value() && !std::isfinite(*latitude)) {
    latitude.reset();
  }
  if (longitude.has_value() && !std::isfinite(*longitude)) {
    longitude.reset();
  }
  if (m_weatherLatitude == latitude && m_weatherLongitude == longitude) {
    return;
  }
  m_weatherLatitude = latitude;
  m_weatherLongitude = longitude;
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

bool GammaService::hasWeatherCoordinates() const {
  return m_weatherLatitude.has_value() && m_weatherLongitude.has_value();
}

GammaService::GeoCoordinates GammaService::scheduleCoordinates() const {
  if (m_config.useWeatherLocation && hasWeatherCoordinates()) {
    return GeoCoordinates{.latitude = m_weatherLatitude, .longitude = m_weatherLongitude};
  }

  if (m_config.latitude.has_value() || m_config.longitude.has_value()) {
    if (m_config.latitude.has_value() && m_config.longitude.has_value()) {
      return GeoCoordinates{.latitude = m_config.latitude, .longitude = m_config.longitude};
    }
    return GeoCoordinates{};
  }

  return GeoCoordinates{};
}

bool GammaService::isManualMode() const {
  return !effectiveForce() && (!m_config.useWeatherLocation || !hasWeatherCoordinates()) &&
         normalizedClock(m_config.startTime).has_value() && normalizedClock(m_config.stopTime).has_value();
}

bool GammaService::isManualNightPhase() const {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  ::localtime_r(&t, &local);
  const int nowMin = local.tm_hour * 60 + local.tm_min;

  const int sunsetMin = timeToMinutes(m_config.startTime);
  const int sunriseMin = timeToMinutes(m_config.stopTime);

  if (sunsetMin < sunriseMin) {
    return nowMin >= sunsetMin && nowMin < sunriseMin;
  }
  return nowMin >= sunsetMin || nowMin < sunriseMin;
}

std::chrono::milliseconds GammaService::msUntilNextManualBoundary() const {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  ::localtime_r(&t, &local);
  const int nowMin = local.tm_hour * 60 + local.tm_min;
  const int nowSec = local.tm_sec;

  const int sunsetMin = timeToMinutes(m_config.startTime);
  const int sunriseMin = timeToMinutes(m_config.stopTime);
  const int targetMin = isManualNightPhase() ? sunriseMin : sunsetMin;

  int diffMin = targetMin - nowMin;
  if (diffMin <= 0) {
    diffMin += 1440;
  }

  const auto ms = std::chrono::milliseconds(diffMin * 60 * 1000 - nowSec * 1000);
  return std::max(ms, std::chrono::milliseconds(1000));
}

void GammaService::scheduleManualTimer() {
  const auto boundaryDelay = msUntilNextManualBoundary();
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

std::optional<std::string> GammaService::normalizedClock(std::string_view value) const {
  if (value.size() != 5 || value[2] != ':') {
    return std::nullopt;
  }
  if (!std::isdigit(static_cast<unsigned char>(value[0])) || !std::isdigit(static_cast<unsigned char>(value[1])) ||
      !std::isdigit(static_cast<unsigned char>(value[3])) || !std::isdigit(static_cast<unsigned char>(value[4]))) {
    return std::nullopt;
  }
  const int hour = (value[0] - '0') * 10 + (value[1] - '0');
  const int minute = (value[3] - '0') * 10 + (value[4] - '0');
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return std::nullopt;
  }
  return std::string(value);
}

// --- Solar position (simplified NOAA) ---

GammaService::SolarTimes GammaService::computeSolarTimes(double lat, double lon) const {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  ::localtime_r(&t, &local);

  constexpr double kPi = std::numbers::pi;
  const double dayOfYear = static_cast<double>(local.tm_yday + 1);
  const double fractionalYear = 2.0 * kPi / 365.0 * (dayOfYear - 1.0);

  const double equationOfTime =
      229.18 * (0.000075 + 0.001868 * std::cos(fractionalYear) - 0.032077 * std::sin(fractionalYear) -
                0.014615 * std::cos(2.0 * fractionalYear) - 0.040849 * std::sin(2.0 * fractionalYear));
  const double declination = 0.006918 - 0.399912 * std::cos(fractionalYear) + 0.070257 * std::sin(fractionalYear) -
                             0.006758 * std::cos(2.0 * fractionalYear) + 0.000907 * std::sin(2.0 * fractionalYear) -
                             0.002697 * std::cos(3.0 * fractionalYear) + 0.00148 * std::sin(3.0 * fractionalYear);

  constexpr double kSunriseZenith = 90.833 * kPi / 180.0;
  const double latRad = lat * kPi / 180.0;
  const double hourAngleArg =
      std::cos(kSunriseZenith) / (std::cos(latRad) * std::cos(declination)) - std::tan(latRad) * std::tan(declination);

  if (hourAngleArg > 1.0) {
    return SolarTimes{.sunriseMinutes = 0, .sunsetMinutes = 0};
  }
  if (hourAngleArg < -1.0) {
    return SolarTimes{.sunriseMinutes = 0, .sunsetMinutes = 1440};
  }

  const double hourAngleDeg = std::acos(std::clamp(hourAngleArg, -1.0, 1.0)) * 180.0 / kPi;
  const double timeZoneOffsetMin = static_cast<double>(local.tm_gmtoff) / 60.0;
  const double solarNoonMin = 720.0 - 4.0 * lon - equationOfTime + timeZoneOffsetMin;

  auto normalizeMinutes = [](double minutes) -> int {
    int rounded = static_cast<int>(std::round(minutes));
    rounded %= 1440;
    if (rounded < 0) {
      rounded += 1440;
    }
    return rounded;
  };

  return SolarTimes{
      .sunriseMinutes = normalizeMinutes(solarNoonMin - hourAngleDeg * 4.0),
      .sunsetMinutes = normalizeMinutes(solarNoonMin + hourAngleDeg * 4.0),
  };
}

bool GammaService::isGeoNightPhase() const {
  const auto coords = scheduleCoordinates();
  if (!coords.latitude.has_value() || !coords.longitude.has_value()) {
    return false;
  }

  const auto times = computeSolarTimes(*coords.latitude, *coords.longitude);
  if (times.sunriseMinutes == 0 && times.sunsetMinutes == 0) {
    return true;
  }
  if (times.sunriseMinutes == 0 && times.sunsetMinutes == 1440) {
    return false;
  }

  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  ::localtime_r(&t, &local);
  const int nowMin = local.tm_hour * 60 + local.tm_min;

  const int sunset = times.sunsetMinutes;
  const int sunrise = times.sunriseMinutes;
  if (sunset > sunrise) {
    return nowMin >= sunset || nowMin < sunrise;
  }
  return nowMin >= sunset && nowMin < sunrise;
}

std::chrono::milliseconds GammaService::msUntilNextGeoBoundary() const {
  const auto coords = scheduleCoordinates();
  if (!coords.latitude.has_value() || !coords.longitude.has_value()) {
    return std::chrono::milliseconds(3600000);
  }

  const auto times = computeSolarTimes(*coords.latitude, *coords.longitude);
  if (times.sunriseMinutes == 0 && (times.sunsetMinutes == 0 || times.sunsetMinutes == 1440)) {
    return std::chrono::milliseconds(3600000);
  }

  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  ::localtime_r(&t, &local);
  const int nowMin = local.tm_hour * 60 + local.tm_min;
  const int nowSec = local.tm_sec;

  const int targetMin = isGeoNightPhase() ? times.sunriseMinutes : times.sunsetMinutes;
  int diffMin = targetMin - nowMin;
  if (diffMin <= 0) {
    diffMin += 1440;
  }

  const auto ms = std::chrono::milliseconds(diffMin * 60 * 1000 - nowSec * 1000);
  return std::max(ms, std::chrono::milliseconds(1000));
}

void GammaService::scheduleGeoTimer() {
  const auto boundaryDelay = msUntilNextGeoBoundary();
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
  if (isManualMode()) {
    return isManualNightPhase();
  }
  return isGeoNightPhase();
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
  const int interpolated = static_cast<int>(
      std::lerp(static_cast<float>(m_transitionFromKelvin), static_cast<float>(m_targetKelvin), m_transitionProgress)
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

  if (isManualMode()) {
    return isManualNightPhase() ? nightTemp : dayTemp;
  }

  // Geo mode - need coordinates.
  const auto coords = scheduleCoordinates();
  if (!coords.latitude.has_value() || !coords.longitude.has_value()) {
    if (!m_config.useWeatherLocation && (m_config.latitude.has_value() || m_config.longitude.has_value())) {
      kLog.warn("need both latitude and longitude when overriding location mode");
    } else if (!m_config.useWeatherLocation) {
      kLog.warn("no schedule: set start_time/stop_time or latitude/longitude, or enable weather location");
    } else if (m_weatherLocationConfigured) {
      kLog.debug("night light schedule waiting for weather location");
    } else {
      kLog.warn(
          "no schedule: configure weather location or disable weather location and set start_time/stop_time or "
          "latitude/longitude"
      );
    }
    return -1;
  }

  return isGeoNightPhase() ? nightTemp : dayTemp;
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

  const bool manualMode = isManualMode();
  if (effectiveEnabled() && manualMode) {
    scheduleManualTimer();
  } else if (effectiveEnabled() && !effectiveForce()) {
    const auto coords = scheduleCoordinates();
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
