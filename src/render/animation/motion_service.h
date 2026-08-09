#pragma once

#include <cstdint>
#include <unordered_set>

class AnimationManager;

class MotionService {
public:
  static MotionService& instance();

  MotionService(const MotionService&) = delete;
  MotionService& operator=(const MotionService&) = delete;

  void registerManager(AnimationManager* manager);
  void unregisterManager(AnimationManager* manager);

  void setEnabled(bool enabled);
  void setSpeed(float speed);

  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] float speed() const noexcept { return m_speed; }

private:
  MotionService() = default;

  bool m_enabled = true;
  float m_speed = 1.0F;
  std::unordered_set<AnimationManager*> m_managers;
};
