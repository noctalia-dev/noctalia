#include "idle/idle_manager.h"

#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <utility>

namespace {

  constexpr Logger kLog("idle");

  constexpr std::uint32_t kHeartbeatTimeoutMs = 1000;
  constexpr double kMaxIdleTimeoutMs = static_cast<double>(std::numeric_limits<std::uint32_t>::max());

  const ext_idle_notification_v1_listener kIdleNotificationListener = {
      .idled = &IdleManager::handleIdled,
      .resumed = &IdleManager::handleResumed,
  };

  const ext_idle_notification_v1_listener kHeartbeatListener = {
      .idled = &IdleManager::handleHeartbeatIdled,
      .resumed = &IdleManager::handleHeartbeatResumed,
  };

  std::uint32_t timeoutSecondsToMilliseconds(double timeoutSeconds) {
    const double timeoutMs = std::clamp(std::ceil(timeoutSeconds * 1000.0), 1.0, kMaxIdleTimeoutMs);
    return static_cast<std::uint32_t>(timeoutMs);
  }

} // namespace

IdleManager::IdleManager() = default;

IdleManager::~IdleManager() {
  clearBehaviors();
  destroyHeartbeat();
}

bool IdleManager::initialize(WaylandConnection& wayland, GraceBeginCallback onBegin, GraceEndCallback onEnd) {
  m_wayland = &wayland;
  m_onGraceBegin = std::move(onBegin);
  m_onGraceEnd = std::move(onEnd);
  if (!m_wayland->hasIdleNotifier()) {
    kLog.info("idle notify protocol unavailable");
  }
  return true;
}

void IdleManager::setActionRunner(ActionRunner runner) { m_actionRunner = std::move(runner); }

void IdleManager::setLiveIdleChangeCallback(std::function<void()> callback) {
  m_onLiveIdleChange = std::move(callback);
}

void IdleManager::notifyLiveIdleChanged() {
  if (m_onLiveIdleChange) {
    m_onLiveIdleChange();
  }
}

void IdleManager::setScreenSaverInhibitLocks(std::int64_t locks) {
  const std::int64_t next = std::max<std::int64_t>(0, locks);
  const bool wasInhibited = m_screenSaverInhibitLocks > 0;
  m_screenSaverInhibitLocks = next;
  const bool inhibited = m_screenSaverInhibitLocks > 0;
  if (!wasInhibited && inhibited) {
    m_heartbeatCompositorIdle = false;
    if (m_liveIdleSeconds != 0) {
      m_liveIdleSeconds = 0;
      notifyLiveIdleChanged();
    }
  }
  if (wasInhibited && !inhibited && m_idledWhileScreenSaverInhibited) {
    m_idledWhileScreenSaverInhibited = false;
    recreateBehaviorNotifications();
  }
}

void IdleManager::reload(const IdleConfig& config) {
  clearBehaviors();
  m_idleConfig = config;

  if (m_wayland == nullptr) {
    destroyHeartbeat();
    return;
  }
  if (!m_wayland->hasIdleNotifier()) {
    kLog.info("idle notify protocol unavailable; idle behaviors not registered");
    destroyHeartbeat();
    return;
  }
  if (m_wayland->seat() == nullptr) {
    kLog.warn("cannot register idle behaviors without a Wayland seat");
    destroyHeartbeat();
    return;
  }

  for (const auto& behavior : config.behaviors) {
    createBehavior(behavior);
  }
  syncHeartbeat();
}

void IdleManager::onSecondTick() {
  if (m_heartbeatCompositorIdle && m_screenSaverInhibitLocks == 0) {
    ++m_liveIdleSeconds;
  }
}

void IdleManager::syncHeartbeat() {
  destroyHeartbeat();
  if (m_wayland == nullptr || !m_wayland->hasIdleNotifier() || m_wayland->seat() == nullptr) {
    m_heartbeatCompositorIdle = false;
    m_liveIdleSeconds = 0;
    return;
  }

  m_heartbeatNotification = m_wayland->createIdleNotification(kHeartbeatTimeoutMs);
  if (m_heartbeatNotification == nullptr) {
    kLog.warn("failed to register idle heartbeat for live status");
    return;
  }

  ext_idle_notification_v1_add_listener(m_heartbeatNotification, &kHeartbeatListener, this);
  kLog.debug("registered idle heartbeat ({}ms)", kHeartbeatTimeoutMs);
}

void IdleManager::destroyHeartbeat() {
  if (m_heartbeatNotification != nullptr) {
    ext_idle_notification_v1_destroy(m_heartbeatNotification);
    m_heartbeatNotification = nullptr;
  }
  m_heartbeatCompositorIdle = false;
  m_liveIdleSeconds = 0;
}

void IdleManager::clearBehaviors() {
  cancelActiveGrace(false);
  for (auto& behavior : m_behaviors) {
    if (behavior->notification != nullptr) {
      ext_idle_notification_v1_destroy(behavior->notification);
      behavior->notification = nullptr;
    }
  }
  m_behaviors.clear();
}

void IdleManager::recreateBehaviorNotification(BehaviorState& behavior) {
  if (m_wayland == nullptr || !m_wayland->hasIdleNotifier() || m_wayland->seat() == nullptr) {
    return;
  }

  if (behavior.notification != nullptr) {
    ext_idle_notification_v1_destroy(behavior.notification);
    behavior.notification = nullptr;
  }
  behavior.phase = BehaviorPhase::Waiting;

  if (!behavior.config.enabled
      || !std::isfinite(behavior.config.timeoutSeconds)
      || behavior.config.timeoutSeconds <= 0.0) {
    return;
  }

  const auto timeoutMs = timeoutSecondsToMilliseconds(behavior.config.timeoutSeconds);
  behavior.notification = m_wayland->createIdleNotification(timeoutMs);
  if (behavior.notification == nullptr) {
    kLog.warn("failed to re-register idle behavior '{}'", behavior.config.name);
    return;
  }
  ext_idle_notification_v1_add_listener(behavior.notification, &kIdleNotificationListener, &behavior);
}

void IdleManager::recreateBehaviorNotifications() {
  if (m_wayland == nullptr || !m_wayland->hasIdleNotifier() || m_wayland->seat() == nullptr) {
    return;
  }

  cancelActiveGrace(false);
  for (auto& behavior : m_behaviors) {
    recreateBehaviorNotification(*behavior);
  }
  kLog.info("idle behavior notifications reset after screensaver inhibit released");
}

void IdleManager::createBehavior(const IdleBehaviorConfig& config) {
  if (!config.enabled) {
    return;
  }
  if (!std::isfinite(config.timeoutSeconds) || config.timeoutSeconds < 0.0) {
    kLog.warn("idle behavior '{}' ignored: timeout must be >= 0 seconds", config.name);
    return;
  }
  if (config.timeoutSeconds == 0.0) {
    kLog.debug("idle behavior '{}' disabled by zero timeout", config.name);
    return;
  }
  const ResolvedIdleBehavior resolved = resolveIdleBehaviorActions(config);
  if (resolved.idleAction.kind == IdleActionKind::None) {
    kLog.warn("idle behavior '{}' ignored: needs an action", config.name);
    return;
  }

  auto behavior = std::make_unique<BehaviorState>();
  behavior->owner = this;
  behavior->config = config;
  const auto timeoutMs = timeoutSecondsToMilliseconds(config.timeoutSeconds);
  behavior->notification = m_wayland->createIdleNotification(timeoutMs);
  if (behavior->notification == nullptr) {
    kLog.warn("failed to register idle behavior '{}'", config.name);
    return;
  }

  ext_idle_notification_v1_add_listener(behavior->notification, &kIdleNotificationListener, behavior.get());
  kLog.info("registered idle behavior '{}' timeout={}s", config.name, config.timeoutSeconds);
  m_behaviors.push_back(std::move(behavior));
}

void IdleManager::runBehavior(BehaviorState& behavior) {
  const ResolvedIdleBehavior resolved = resolveIdleBehaviorActions(behavior.config);
  if (!runAction(behavior.config, resolved.idleAction)) {
    kLog.warn("idle behavior '{}' action failed", behavior.config.name);
  }
}

void IdleManager::runResumeBehavior(BehaviorState& behavior) {
  const ResolvedIdleBehavior resolved = resolveIdleBehaviorActions(behavior.config);
  if (resolved.resumeAction.kind == IdleActionKind::None) {
    return;
  }
  if (!runAction(behavior.config, resolved.resumeAction)) {
    kLog.warn("idle behavior '{}' resume action failed", behavior.config.name);
  }
}

bool IdleManager::runAction(const IdleBehaviorConfig& behavior, const IdleActionRequest& action) const {
  if (action.kind == IdleActionKind::None) {
    return true;
  }
  if (!m_actionRunner) {
    return false;
  }
  return m_actionRunner(behavior, action);
}

void IdleManager::cancelActiveGrace(bool userCancelled) {
  if (!hasActiveGrace()) {
    return;
  }
  for (auto* behavior : m_graceBehaviors) {
    if (behavior != nullptr) {
      behavior->phase = BehaviorPhase::Waiting;
    }
  }
  m_graceBehaviors.clear();
  m_graceFallbackTimer.stop();
  ++m_graceGeneration;
  if (m_onGraceEnd) {
    m_onGraceEnd(userCancelled);
  }
}

void IdleManager::graceFadeComplete() {
  if (!hasActiveGrace()) {
    return;
  }
  auto behaviors = std::move(m_graceBehaviors);
  m_graceBehaviors.clear();
  m_graceFallbackTimer.stop();
  m_graceFallbackTimer.stop();
  if (m_onGraceEnd) {
    m_onGraceEnd(false);
  }
  for (auto* behavior : behaviors) {
    if (behavior == nullptr || behavior->phase != BehaviorPhase::Fading) {
      continue;
    }
    behavior->phase = BehaviorPhase::Idled;
    kLog.info("idle behavior '{}' triggered after pre-action fade", behavior->config.name);
    runBehavior(*behavior);
  }
}

void IdleManager::joinActiveGrace(BehaviorState& behavior) {
  if (std::ranges::contains(m_graceBehaviors, &behavior)) {
    return;
  }
  behavior.phase = BehaviorPhase::Fading;
  m_graceBehaviors.push_back(&behavior);
}

void IdleManager::handleIdled(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* behavior = static_cast<BehaviorState*>(data);
  if (behavior == nullptr || behavior->owner == nullptr) {
    return;
  }
  if (behavior->phase != BehaviorPhase::Waiting) {
    return;
  }

  IdleManager& self = *behavior->owner;
  if (self.m_screenSaverInhibitLocks > 0) {
    self.m_idledWhileScreenSaverInhibited = true;
    kLog.info(
        "idle behavior '{}' suppressed (screensaver inhibit locks={})", behavior->config.name,
        self.m_screenSaverInhibitLocks
    );
    self.recreateBehaviorNotification(*behavior);
    return;
  }

  const float fadeSec = self.m_idleConfig.preActionFadeSeconds;
  if (fadeSec > 0.0005f) {
    assert(self.m_onGraceBegin);
    if (self.hasActiveGrace()) {
      self.joinActiveGrace(*behavior);
      kLog.info("idle behavior '{}' joined active pre-action fade", behavior->config.name);
      return;
    }

    const int fadeMs = static_cast<int>(std::lround(static_cast<double>(fadeSec) * 1000.0));
    const auto fade = std::chrono::milliseconds(std::clamp(fadeMs, 1, 600000));
    kLog.info("idle behavior '{}' pre-action fade {}ms", behavior->config.name, fade.count());
    const IdleActionKind idleKind = resolveIdleBehaviorActions(behavior->config).idleAction.kind;
    const bool willLockSession = idleKind == IdleActionKind::Lock || idleKind == IdleActionKind::LockAndSuspend;
    self.joinActiveGrace(*behavior);
    const std::uint64_t generation = ++self.m_graceGeneration;
    self.m_graceFallbackTimer.start(fade + std::chrono::milliseconds(250), [&self, generation]() {
      if (self.m_graceGeneration != generation || !self.hasActiveGrace()) {
        return;
      }
      kLog.debug("idle pre-action fade fallback completed");
      self.graceFadeComplete();
    });
    self.m_onGraceBegin(behavior->config.name, fade, willLockSession, [ptr = &self, generation]() {
      DeferredCall::callLater([ptr, generation]() {
        if (ptr->m_graceGeneration != generation || !ptr->hasActiveGrace()) {
          return;
        }
        ptr->graceFadeComplete();
      });
    });
    return;
  }

  behavior->phase = BehaviorPhase::Idled;
  kLog.info("idle behavior '{}' triggered", behavior->config.name);
  self.runBehavior(*behavior);
}

void IdleManager::handleResumed(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* behavior = static_cast<BehaviorState*>(data);
  if (behavior == nullptr || behavior->owner == nullptr) {
    return;
  }

  IdleManager& self = *behavior->owner;
  if (behavior->phase == BehaviorPhase::Fading) {
    kLog.info("idle behavior '{}' pre-action fade cancelled (user active)", behavior->config.name);
    self.cancelActiveGrace(true);
    return;
  }

  if (behavior->phase != BehaviorPhase::Idled) {
    return;
  }

  behavior->phase = BehaviorPhase::Waiting;
  kLog.info("idle behavior '{}' resumed", behavior->config.name);
  self.runResumeBehavior(*behavior);
}

void IdleManager::handleHeartbeatIdled(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* self = static_cast<IdleManager*>(data);
  if (self == nullptr) {
    return;
  }
  if (self->m_screenSaverInhibitLocks > 0) {
    return;
  }
  self->m_heartbeatCompositorIdle = true;
  self->m_liveIdleSeconds = 1;
  self->notifyLiveIdleChanged();
}

void IdleManager::handleHeartbeatResumed(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* self = static_cast<IdleManager*>(data);
  if (self == nullptr) {
    return;
  }
  self->m_heartbeatCompositorIdle = false;
  self->m_liveIdleSeconds = 0;
  self->notifyLiveIdleChanged();
}
