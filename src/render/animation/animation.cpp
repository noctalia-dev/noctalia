#include "render/animation/animation.h"

#include <algorithm>

float applyEasing(Easing easing, float t) {
  t = std::clamp(t, 0.0F, 1.0F);

  switch (easing) {
  case Easing::Linear:
    return t;

  case Easing::EaseInQuad:
    return t * t;

  case Easing::EaseOutQuad:
    return t * (2.0F - t);

  case Easing::EaseInOutQuad:
    if (t < 0.5F) {
      return 2.0F * t * t;
    }
    return -1.0F + (4.0F - 2.0F * t) * t;

  case Easing::EaseOutCubic: {
    const float f = t - 1.0F;
    return f * f * f + 1.0F;
  }

  case Easing::EaseInOutCubic:
    if (t < 0.5F) {
      return 4.0F * t * t * t;
    } else {
      const float f = 2.0F * t - 2.0F;
      return 0.5F * f * f * f + 1.0F;
    }

  case Easing::EaseOutBack: {
    constexpr float c1 = 1.70158F;
    constexpr float c3 = c1 + 1.0F;
    const float f = t - 1.0F;
    return 1.0F + c3 * f * f * f + c1 * f * f;
  }
  }

  return t;
}
