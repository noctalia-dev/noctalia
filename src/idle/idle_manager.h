#pragma once

#include "config/config_service.h"
#include "core/timer_manager.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class WaylandConnection;
struct ext_idle_notification_v1;

class IdleManager {
public:
  /// Runs the resolved idle or resume action for this behavior.
  using ActionRunner = std::function<bool(const IdleBehaviorConfig& behavior, const IdleActionRequest& action)>;
  /// Starts the pre-action fade overlay; call `onFadeComplete` once every output has finished fading.
  using GraceBeginCallback = std::function<void(
      const std::string& behaviorName, std::chrono::milliseconds fadeDuration, std::function<void()> onFadeComplete
  )>;
  /// `userCancelled` is true when input resumed during the fade before the idle action ran.
  using GraceEndCallback = std::function<void(bool userCancelled)>;

  IdleManager();
  ~IdleManager();

  IdleManager(const IdleManager&) = delete;
  IdleManager& operator=(const IdleManager&) = delete;

  bool initialize(WaylandConnection& wayland, GraceBeginCallback onBegin, GraceEndCallback onEnd);
  void setActionRunner(ActionRunner runner);
  void setLiveIdleChangeCallback(std::function<void()> callback);
  void reload(const IdleConfig& config);
  /// Seconds the compositor has reported session-idle (1s heartbeat notification); 0 when active.
  [[nodiscard]] std::int64_t liveIdleSeconds() const noexcept { return m_liveIdleSeconds; }
  void onSecondTick();
  static void handleIdled(void* data, ext_idle_notification_v1* notification);
  static void handleResumed(void* data, ext_idle_notification_v1* notification);
  static void handleHeartbeatIdled(void* data, ext_idle_notification_v1* notification);
  static void handleHeartbeatResumed(void* data, ext_idle_notification_v1* notification);

private:
  enum class BehaviorPhase : std::uint8_t {
    Waiting,
    Fading,
    Idled,
  };

  struct BehaviorState {
    IdleManager* owner = nullptr;
    IdleBehaviorConfig config;
    ext_idle_notification_v1* notification = nullptr;
    BehaviorPhase phase = BehaviorPhase::Waiting;
  };

  void clearBehaviors();
  void syncHeartbeat();
  void destroyHeartbeat();
  void notifyLiveIdleChanged();
  void createBehavior(const IdleBehaviorConfig& config);
  void runBehavior(BehaviorState& behavior);
  void runResumeBehavior(BehaviorState& behavior);
  bool runAction(const IdleBehaviorConfig& behavior, const IdleActionRequest& action) const;
  void cancelActiveGrace(bool userCancelled);
  void graceFadeComplete();
  void joinActiveGrace(BehaviorState& behavior);
  [[nodiscard]] bool hasActiveGrace() const noexcept { return !m_graceBehaviors.empty(); }

  WaylandConnection* m_wayland = nullptr;
  ActionRunner m_actionRunner;
  GraceBeginCallback m_onGraceBegin;
  GraceEndCallback m_onGraceEnd;
  std::function<void()> m_onLiveIdleChange;
  IdleConfig m_idleConfig;
  Timer m_graceFallbackTimer;
  std::vector<BehaviorState*> m_graceBehaviors;
  std::uint64_t m_graceGeneration = 0;
  std::vector<std::unique_ptr<BehaviorState>> m_behaviors;
  ext_idle_notification_v1* m_heartbeatNotification = nullptr;
  bool m_heartbeatCompositorIdle = false;
  std::int64_t m_liveIdleSeconds = 0;
};
