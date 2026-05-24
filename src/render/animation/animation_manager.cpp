#include "render/animation/animation_manager.h"

#include "render/animation/motion_service.h"

#include <chrono>

namespace {
  constexpr float kReducedMotionDurationMs = 1.0f;
}

AnimationManager::AnimationManager() { MotionService::instance().registerManager(this); }

AnimationManager::~AnimationManager() { MotionService::instance().unregisterManager(this); }

AnimationManager::Id AnimationManager::animate(
    float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner
) {
  return animateInternal(from, to, durationMs, easing, std::move(setter), std::move(onComplete), owner, true, true);
}

AnimationManager::Id AnimationManager::animateUnscaled(
    float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner
) {
  return animateInternal(from, to, durationMs, easing, std::move(setter), std::move(onComplete), owner, false, true);
}

AnimationManager::Id AnimationManager::animateTimer(
    float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner
) {
  return animateInternal(from, to, durationMs, easing, std::move(setter), std::move(onComplete), owner, false, false);
}

AnimationManager::Id AnimationManager::animateInternal(
    float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
    std::function<void()> onComplete, const void* owner, bool scaleDuration, bool respectMotionEnabled
) {
  const auto& motion = MotionService::instance();
  const bool reduceMotion = respectMotionEnabled && !motion.enabled();
  if (reduceMotion) {
    if (setter) {
      setter(to);
    }
    if (!onComplete) {
      return 0;
    }
  }

  float effectiveDurationMs = scaleDuration ? durationMs / motion.speed() : durationMs;
  if (reduceMotion) {
    effectiveDurationMs = kReducedMotionDurationMs;
  }

  if (effectiveDurationMs <= 0.0f) {
    if (setter) {
      setter(to);
    }
    if (onComplete) {
      onComplete();
    }
    return 0;
  }

  Id id = m_nextId++;
  const auto now = std::chrono::steady_clock::now();
  m_animations.push_back(
      Entry{
          .id = id,
          .owner = owner,
          .respectMotionEnabled = respectMotionEnabled,
          .animation = Animation{
              .startValue = reduceMotion ? to : from,
              .endValue = to,
              .durationMs = effectiveDurationMs,
              .startedAt = reduceMotion ? now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                    std::chrono::duration<float, std::milli>(kReducedMotionDurationMs)
                                                )
                                        : now,
              .easing = easing,
              .setter = std::move(setter),
              .onComplete = std::move(onComplete),
          },
      }
  );
  return id;
}

void AnimationManager::cancel(Id id) {
  std::erase_if(m_animations, [id](const Entry& e) { return e.id == id; });
}

void AnimationManager::cancelAll() { m_animations.clear(); }

void AnimationManager::reduceMotion() {
  const auto now = std::chrono::steady_clock::now();
  for (auto& entry : m_animations) {
    if (!entry.respectMotionEnabled || entry.animation.finished) {
      continue;
    }

    auto& anim = entry.animation;
    if (anim.setter) {
      anim.setter(anim.endValue);
    }
    anim.startValue = anim.endValue;
    anim.durationMs = kReducedMotionDurationMs;
    anim.elapsedMs = 0.0f;
    anim.startedAt = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                               std::chrono::duration<float, std::milli>(kReducedMotionDurationMs)
                           );
  }
}

void AnimationManager::cancelForOwner(const void* owner) {
  if (owner == nullptr) {
    return;
  }
  std::erase_if(m_animations, [owner](const Entry& e) { return e.owner == owner; });
}

void AnimationManager::tick(float /*deltaMs*/) {
  // Collect completed callbacks separately — onComplete may call animate()
  // which would push_back and invalidate iterators during iteration.
  std::vector<std::function<void()>> completedCallbacks;

  const auto now = std::chrono::steady_clock::now();

  for (auto& entry : m_animations) {
    auto& anim = entry.animation;
    if (anim.finished) {
      continue;
    }

    // Progress from wall time so a fixed duration stays correct when the compositor
    // delivers sparse `wl_surface.frame` callbacks (common right after cold boot).
    const float wallElapsedMs = std::chrono::duration<float, std::milli>(now - anim.startedAt).count();
    anim.elapsedMs = wallElapsedMs;
    float t = anim.durationMs > 0.0f ? wallElapsedMs / anim.durationMs : 1.0f;

    if (t >= 1.0f) {
      t = 1.0f;
      anim.finished = true;
    }

    float easedT = applyEasing(anim.easing, t);
    float value = anim.startValue + (anim.endValue - anim.startValue) * easedT;

    if (anim.setter) {
      anim.setter(value);
    }

    if (anim.finished && anim.onComplete) {
      completedCallbacks.push_back(std::move(anim.onComplete));
    }
  }

  std::erase_if(m_animations, [](const Entry& e) { return e.animation.finished; });

  for (auto& cb : completedCallbacks) {
    cb();
  }
}

bool AnimationManager::hasActive() const { return !m_animations.empty(); }
