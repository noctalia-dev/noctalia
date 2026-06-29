#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

enum class Easing : std::uint8_t {
  Linear,
  EaseInQuad,
  EaseOutQuad,
  EaseInOutQuad,
  EaseOutCubic,
  EaseInOutCubic,
  EaseOutBack,
};

float applyEasing(Easing easing, float t);

struct Animation {
  float startValue = 0.0f;
  float endValue = 0.0f;
  float durationMs = 0.0f;
  float elapsedMs = 0.0f;
  std::chrono::steady_clock::time_point startedAt;
  Easing easing = Easing::EaseOutQuad;
  std::function<void(float)> setter;
  std::function<void()> onComplete;
  bool finished = false;
};
