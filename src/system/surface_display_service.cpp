#include "system/surface_display_service.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "core/process/process.h"
#include "shell/osd/brightness_osd.h"
#include "system/brightness_service.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {
  Logger kLog{"surface-display"};

  // Sample accel often for responsive rotate; only the niri transform itself is rate-limited.
  constexpr auto kBrightnessInterval = std::chrono::milliseconds(1000);
  constexpr auto kRotateSampleInterval = std::chrono::milliseconds(200);
  constexpr auto kCoverPollInterval = std::chrono::milliseconds(750);
  constexpr auto kAlsInactiveBackoff = std::chrono::seconds(30);
  // Short enough to feel snappy after a flip; long enough to avoid transform storms.
  constexpr auto kRotateApplyCooldown = std::chrono::milliseconds(750);
  constexpr auto kBrightnessRampDuration = std::chrono::milliseconds(750);

  [[nodiscard]] float brightnessFromLux(double lux, const SurfaceConfig& config, float minimumBrightness) {
    const float floor = std::clamp(minimumBrightness, 0.0F, 1.0F);
    const double dark = std::max(0.01, config.autoBrightnessDarkLux);
    const double bright = std::max(dark * 1.5, config.autoBrightnessBrightLux);
    const double luxClamped = std::max(lux, 0.01);
    const double t = std::clamp(
        (std::log(luxClamped) - std::log(dark)) / (std::log(bright) - std::log(dark)), 0.0, 1.0
    );
    return static_cast<float>(static_cast<double>(floor) + (1.0 - static_cast<double>(floor)) * t);
  }

  [[nodiscard]] std::string pickBuiltinOutput(const CompositorPlatform& platform) {
    for (const auto& output : platform.outputs()) {
      if (output.connectorName.starts_with("eDP") || output.connectorName.starts_with("DSI")
          || output.connectorName.starts_with("LVDS")) {
        return output.connectorName;
      }
    }
    if (!platform.outputs().empty() && !platform.outputs().front().connectorName.empty()) {
      return platform.outputs().front().connectorName;
    }
    return {};
  }
} // namespace

SurfaceDisplayService::SurfaceDisplayService(
    CompositorPlatform& platform, BrightnessService* brightness, BrightnessOsd* brightnessOsd, SurfaceConfig config,
    float brightnessMinimum, bool surfaceHost
)
    : m_platform(platform)
    , m_brightness(brightness)
    , m_brightnessOsd(brightnessOsd)
    , m_config(std::move(config))
    , m_brightnessMinimum(brightnessMinimum)
    , m_surfaceHost(surfaceHost) {
  applySlateAsTablet();
  syncCoverAndOsk();
  syncTimer();
}

SurfaceDisplayService::~SurfaceDisplayService() {
  m_timer.stop();
  m_brightnessRampTimer.stop();
  disarmOsk("shutdown");
}

void SurfaceDisplayService::applySlateAsTablet() {
  // Slate-as-tablet is a Niri Surface Display setting; only apply there.
  if (!m_surfaceHost || !compositors::isNiri()) {
    return;
  }
  const auto current = noctalia::system::surface::readTabletModeInSlateState();
  if (!current.has_value()) {
    return;
  }
  // Unprivileged hosts cannot write this sysfs param (root:root 0644). Settings hide the
  // toggle when not writable — leave the kernel value alone and stay quiet.
  if (!noctalia::system::surface::tabletModeInSlateStateWritable()) {
    return;
  }
  if (*current == m_config.slateAsTablet) {
    return;
  }
  if (!noctalia::system::surface::writeTabletModeInSlateState(m_config.slateAsTablet)) {
    if (!m_loggedSlateWriteFailure) {
      kLog.warn("failed to write tablet_mode_in_slate_state={}", m_config.slateAsTablet);
      m_loggedSlateWriteFailure = true;
    }
  }
}

void SurfaceDisplayService::disarmOsk(std::string_view reason) {
  if (!m_oskArmed) {
    return;
  }
  noctalia::system::surface::setWaylandOskArmed(false);
  m_oskArmed = false;
  kLog.info("OSK disarmed ({})", reason);
}

void SurfaceDisplayService::reload(const SurfaceConfig& config, float brightnessMinimum, bool surfaceHost) {
  m_config = config;
  m_brightnessMinimum = brightnessMinimum;
  m_surfaceHost = surfaceHost;
  m_brightnessRampTimer.stop();
  m_lastAppliedBrightness = -1.0F;
  m_settledBrightness = -1.0F;
  m_lastGoodLux = 0.0;
  m_filteredLux = 0.0;
  m_loggedAlsInactive = false;
  m_alsInactive = false;
  m_loggedSlateWriteFailure = false;
  m_consecutiveZeroLux = 0;
  m_stableCount = 0;
  m_pendingRotation = noctalia::system::surface::ScreenRotation::Flat;
  m_nextRotateAllowed = {};
  m_lastBrightnessSample = {};
  m_lastCoverSample = {};
  m_coverAttached.reset();
  if (!m_config.coverDetachAwareness || !m_config.oskOnCoverDetach || !m_surfaceHost
      || !compositors::isNiri() || !noctalia::system::surface::waylandOskAvailable()) {
    disarmOsk("reload");
  }
  applySlateAsTablet();
  syncCoverAndOsk();
  syncTimer();
}

bool SurfaceDisplayService::niriDisplayFeaturesActive() const noexcept {
  return m_surfaceHost && compositors::isNiri() && (m_config.autoRotate || m_config.coverDetachAwareness);
}

bool SurfaceDisplayService::featuresActive() const noexcept {
  return (m_surfaceHost && m_config.autoBrightness) || niriDisplayFeaturesActive();
}

void SurfaceDisplayService::syncTimer() {
  if (!featuresActive()) {
    m_timer.stop();
    return;
  }

  std::chrono::milliseconds interval = kBrightnessInterval;
  const bool rotating = m_surfaceHost && compositors::isNiri() && m_config.autoRotate;
  const bool coverOnly =
      niriDisplayFeaturesActive() && !m_config.autoRotate && m_config.coverDetachAwareness;

  if (rotating) {
    // Fast accel sampling; transform apply is separately cooldown-limited.
    interval = kRotateSampleInterval;
  } else if (coverOnly) {
    interval = kCoverPollInterval;
  } else if (m_config.autoBrightness && m_alsInactive) {
    interval = kAlsInactiveBackoff;
  }

  m_timer.startRepeating(interval, [this]() { tick(); });
}

void SurfaceDisplayService::tick() {
  if (!featuresActive()) {
    disarmOsk("features-inactive");
    m_timer.stop();
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const bool wasAlsInactive = m_alsInactive;

  if (m_config.autoBrightness) {
    // When sharing a fast rotate timer, don't thrash ALS every 200ms.
    if (m_lastBrightnessSample == std::chrono::steady_clock::time_point{}
        || now - m_lastBrightnessSample >= kBrightnessInterval) {
      applyAutoBrightness();
      m_lastBrightnessSample = now;
    }
  }

  if (niriDisplayFeaturesActive()) {
    if (m_lastCoverSample == std::chrono::steady_clock::time_point{}
        || now - m_lastCoverSample >= kCoverPollInterval) {
      syncCoverAndOsk();
      m_lastCoverSample = now;
    }
    if (m_config.autoRotate) {
      applyAutoRotate();
    }
  } else {
    disarmOsk("niri-display-inactive");
  }

  // Reschedule when ALS inactivity changes the desired interval.
  if (wasAlsInactive != m_alsInactive) {
    syncTimer();
  }
}

void SurfaceDisplayService::applyAutoBrightness() {
  if (m_brightness == nullptr || !m_brightness->available()) {
    return;
  }
  const auto als = noctalia::system::surface::readAls();
  if (!als.devicePresent) {
    m_alsInactive = false;
    m_consecutiveZeroLux = 0;
    return;
  }

  constexpr std::int32_t kZeroLuxGraceTicks = 8;

  double lux = 0.0;
  if (als.reporting && als.lux.has_value() && *als.lux > 0.0) {
    lux = *als.lux;
    m_lastGoodLux = lux;
    m_consecutiveZeroLux = 0;
    m_alsInactive = false;
    m_loggedAlsInactive = false;
  } else if (m_lastGoodLux > 0.0) {
    ++m_consecutiveZeroLux;
    lux = m_lastGoodLux;
    if (m_consecutiveZeroLux >= kZeroLuxGraceTicks) {
      m_alsInactive = true;
    }
  } else {
    ++m_consecutiveZeroLux;
    if (m_consecutiveZeroLux < kZeroLuxGraceTicks) {
      return;
    }
    m_alsInactive = true;
    if (!m_loggedAlsInactive) {
      kLog.warn(
          "auto-brightness: ALS present but illuminance stays at 0 — backing off until it reports"
      );
      m_loggedAlsInactive = true;
    }
    return;
  }

  const double luxClamped = std::max(lux, 0.01);
  if (m_filteredLux <= 0.0) {
    m_filteredLux = luxClamped;
  } else {
    constexpr double kLuxEma = 0.45;
    m_filteredLux =
        std::exp((1.0 - kLuxEma) * std::log(m_filteredLux) + kLuxEma * std::log(luxClamped));
  }

  const float target = brightnessFromLux(m_filteredLux, m_config, m_brightnessMinimum);

  // Mid-ramp: only retarget the destination if lux moved enough — don't restart.
  if (m_brightnessRampTimer.active()) {
    if (std::abs(target - m_rampTarget) >= 0.05F) {
      m_rampFrom = m_lastAppliedBrightness >= 0.0F ? m_lastAppliedBrightness : m_rampFrom;
      m_rampTarget = target;
      m_rampStartedAt = std::chrono::steady_clock::now();
      kLog.debug("auto-brightness: retarget -> {:.0f}%", target * 100.0F);
    }
    return;
  }

  float current = m_settledBrightness;
  if (current < 0.0F) {
    const auto& displays = m_brightness->displays();
    current = displays.empty() ? target : displays.front().brightness;
    m_settledBrightness = current;
    m_lastAppliedBrightness = current;
  }

  // Ignore tiny lux noise — one intentional transition when the room actually changes.
  constexpr float kCommitDelta = 0.06F;
  if (std::abs(target - current) < kCommitDelta) {
    return;
  }

  kLog.debug(
      "auto-brightness: lux={:.1f} filtered={:.1f} ramp {:.0f}% -> {:.0f}%", lux, m_filteredLux, current * 100.0F,
      target * 100.0F
  );
  startBrightnessRamp(current, target);
}

void SurfaceDisplayService::startBrightnessRamp(float from, float to) {
  m_rampFrom = from;
  m_rampTarget = to;
  m_rampStartedAt = std::chrono::steady_clock::now();
  if (!m_config.autoBrightnessOsd && m_brightnessOsd != nullptr) {
    // Cover the whole ramp plus a beat so the final bar refresh does not pop the OSD.
    m_brightnessOsd->suppressFor(kBrightnessRampDuration + std::chrono::milliseconds(400));
  }
  m_brightnessRampTimer.startRepeating(std::chrono::milliseconds(32), [this]() { brightnessRampTick(); });
  brightnessRampTick();
}

void SurfaceDisplayService::brightnessRampTick() {
  if (m_brightness == nullptr) {
    m_brightnessRampTimer.stop();
    return;
  }

  const auto elapsed = std::chrono::steady_clock::now() - m_rampStartedAt;
  const float t = std::clamp(
      std::chrono::duration<float>(elapsed).count()
          / std::chrono::duration<float>(kBrightnessRampDuration).count(),
      0.0F, 1.0F
  );
  // Smoothstep — ease in/out so the single transition feels continuous.
  const float eased = t * t * (3.0F - 2.0F * t);
  const float value = m_rampFrom + (m_rampTarget - m_rampFrom) * eased;

  if (t >= 1.0F) {
    if (!m_config.autoBrightnessOsd && m_brightnessOsd != nullptr) {
      m_brightnessOsd->suppressFor(std::chrono::milliseconds(500));
    }
    // notify=true refreshes the bar; OSD is suppressed unless auto_brightness_osd is on.
    m_brightness->setAllBrightness(m_rampTarget, true);
    m_lastAppliedBrightness = m_rampTarget;
    m_settledBrightness = m_rampTarget;
    m_brightnessRampTimer.stop();
    return;
  }

  // Silent steps: no bar/OSD refresh until the ramp finishes.
  m_brightness->setAllBrightness(value, false);
  m_lastAppliedBrightness = value;
}

bool SurfaceDisplayService::shouldAutoRotate() const {
  if (!m_config.autoRotate || !compositors::isNiri()) {
    return false;
  }
  if (m_config.autoRotateInLaptopMode) {
    return true;
  }

  if (m_config.coverDetachAwareness) {
    const auto cover = noctalia::system::surface::readTypeCoverAttached();
    if (cover.has_value() && !*cover) {
      // Cover detached → tablet-like.
      return true;
    }
  }

  const auto mode = noctalia::system::surface::readTabletModeState();
  if (!mode.has_value()) {
    // No posture switch → do not guess; rotating in laptop mode via niri is laggy.
    return false;
  }
  return *mode == "tablet" || *mode == "slate";
}

void SurfaceDisplayService::syncCoverAndOsk() {
  if (!compositors::isNiri() || !m_surfaceHost) {
    disarmOsk("not-niri-or-surface");
    return;
  }
  if (!m_config.coverDetachAwareness) {
    disarmOsk("cover-awareness-off");
    return;
  }

  const auto cover = noctalia::system::surface::readTypeCoverAttached();
  if (!cover.has_value()) {
    disarmOsk("cover-unknown");
    return;
  }

  const bool attached = *cover;
  const bool sawTransition = m_coverAttached.has_value() && *m_coverAttached != attached;
  const bool attachedNow = sawTransition && attached;
  m_coverAttached = attached;

  if (attachedNow) {
    // Cover reattached → return the panel to normal orientation.
    setOutputTransform("normal");
    m_appliedRotation = noctalia::system::surface::ScreenRotation::Normal;
    m_hasAppliedRotation = true;
    m_stableCount = 0;
    kLog.info("cover attached: reset niri transform to normal");
  }

  const bool wantOsk =
      m_config.oskOnCoverDetach && !attached && noctalia::system::surface::waylandOskAvailable();
  if (wantOsk) {
    if (!m_oskArmed) {
      noctalia::system::surface::setWaylandOskArmed(true);
      m_oskArmed = true;
      kLog.info("cover detached: OSK armed for text-input");
    }
    return;
  }
  disarmOsk(attached ? "cover-attached" : "osk-disabled");
}

void SurfaceDisplayService::applyAutoRotate() {
  if (!shouldAutoRotate()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (m_nextRotateAllowed != std::chrono::steady_clock::time_point{} && now < m_nextRotateAllowed) {
    return;
  }

  const auto accel = noctalia::system::surface::readAccel();
  if (!accel.has_value()) {
    return;
  }
  using noctalia::system::surface::ScreenRotation;
  const auto rotation = noctalia::system::surface::classifyScreenRotation(*accel);
  if (rotation == ScreenRotation::Flat) {
    m_stableCount = 0;
    return;
  }
  if (rotation == m_pendingRotation) {
    ++m_stableCount;
  } else {
    m_pendingRotation = rotation;
    m_stableCount = 1;
  }
  const auto needed = std::max<std::int32_t>(1, m_config.autoRotateStableSamples);
  if (m_stableCount < needed) {
    return;
  }
  // First observation of "normal" at rest: record it, don't poke the compositor.
  if (!m_hasAppliedRotation && m_pendingRotation == ScreenRotation::Normal) {
    m_appliedRotation = m_pendingRotation;
    m_hasAppliedRotation = true;
    return;
  }
  if (m_pendingRotation == m_appliedRotation && m_hasAppliedRotation) {
    return;
  }
  setOutputTransform(noctalia::system::surface::screenRotationLabel(m_pendingRotation));
  m_appliedRotation = m_pendingRotation;
  m_hasAppliedRotation = true;
  m_nextRotateAllowed = now + kRotateApplyCooldown;
}

void SurfaceDisplayService::setOutputTransform(std::string_view transform) {
  const std::string output = pickBuiltinOutput(m_platform);
  if (output.empty()) {
    kLog.warn("auto-rotate: no output found");
    return;
  }

  if (compositors::isNiri() && process::commandExists("niri")) {
    if (process::runAsync({"niri", "msg", "output", output, "transform", std::string(transform)})) {
      kLog.info("auto-rotate: niri {} -> {}", output, transform);
      return;
    }
  }

  kLog.warn("auto-rotate: niri transform failed for {}", output);
}
