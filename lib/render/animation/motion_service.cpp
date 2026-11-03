#include "render/animation/motion_service.h"

#include "render/animation/animation_manager.h"

#include <algorithm>
#include <vector>

MotionService& MotionService::instance() {
  static MotionService service;
  return service;
}

void MotionService::registerManager(AnimationManager* manager) {
  if (manager == nullptr) {
    return;
  }
  m_managers.insert(manager);
}

void MotionService::unregisterManager(AnimationManager* manager) {
  if (manager == nullptr) {
    return;
  }
  m_managers.erase(manager);
}

void MotionService::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (!m_enabled) {
    const std::vector<AnimationManager*> managers(m_managers.begin(), m_managers.end());
    for (auto* manager : managers) {
      if (manager != nullptr && m_managers.contains(manager)) {
        manager->reduceMotion();
      }
    }
  }
}

void MotionService::setSpeed(float speed) { m_speed = std::clamp(speed, 0.05F, 4.0F); }
