#include "render/animation/animation.h"

#include <algorithm>

float applyEasing(Easing easing, float t) {
  t = std::clamp(t, 0.0f, 1.0f);

  switch (easing) {
  case Easing::Linear:
    return t;

  case Easing::EaseInQuad:
    return t * t;

  case Easing::EaseOutQuad:
    return t * (2.0f - t);

  case Easing::EaseInOutQuad:
    if (t < 0.5f) {
      return 2.0f * t * t;
    }
    return -1.0f + ((4.0f - (2.0f * t)) * t);

  case Easing::EaseOutCubic: {
    const float f = t - 1.0f;
    return (f * f * f) + 1.0f;
  }

  case Easing::EaseInOutCubic:
    if (t < 0.5f) {
      return 4.0f * t * t * t;
    } else {
      const float f = (2.0f * t) - 2.0f;
      return (0.5f * f * f * f) + 1.0f;
    }

  case Easing::EaseOutBack: {
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float f = t - 1.0f;
    return 1.0f + (c3 * f * f * f) + (c1 * f * f);
  }
  }

  return t;
}
