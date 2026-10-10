#include "render/animation/animation.h"

#include <cmath>

float evaluateCubicBezier(float x1, float y1, float x2, float y2, float t) {
  t = std::clamp(t, 0.0F, 1.0F);
  if (t <= 0.0F) {
    return 0.0F;
  }
  if (t >= 1.0F) {
    return 1.0F;
  }

  // Unit cubic bezier parameterized by u in [0, 1]:
  // x(u) = 3*(1-u)^2*u*x1 + 3*(1-u)*u^2*x2 + u^3
  //      = (3*x1 - 3*x2 + 1)*u^3 + (-6*x1 + 3*x2)*u^2 + (3*x1)*u
  const float cx = 3.0F * x1;
  const float bx = 3.0F * (x2 - x1) - cx;
  const float ax = 1.0F - cx - bx;

  const float cy = 3.0F * y1;
  const float by = 3.0F * (y2 - y1) - cy;
  const float ay = 1.0F - cy - by;

  auto sampleCurveX = [ax, bx, cx](float u) noexcept {
    return ((ax * u + bx) * u + cx) * u;
  };
  auto sampleCurveDerivativeX = [ax, bx, cx](float u) noexcept {
    return (3.0F * ax * u + 2.0F * bx) * u + cx;
  };
  auto sampleCurveY = [ay, by, cy](float u) noexcept {
    return ((ay * u + by) * u + cy) * u;
  };

  // Solve for u such that sampleCurveX(u) == t using Newton-Raphson
  float u = t;
  for (int i = 0; i < 8; ++i) {
    const float x = sampleCurveX(u) - t;
    if (std::abs(x) < 1e-5F) {
      return std::clamp(sampleCurveY(u), 0.0F, 1.0F);
    }
    const float d = sampleCurveDerivativeX(u);
    if (std::abs(d) < 1e-6F) {
      break;
    }
    u -= x / d;
    u = std::clamp(u, 0.0F, 1.0F);
  }

  // Fallback bisection if Newton did not converge closely enough
  float low = 0.0F;
  float high = 1.0F;
  u = t;
  while (low < high) {
    const float x = sampleCurveX(u);
    if (std::abs(x - t) < 1e-4F) {
      break;
    }
    if (t > x) {
      low = u;
    } else {
      high = u;
    }
    u = 0.5F * (high + low);
    if (high - low < 1e-4F) {
      break;
    }
  }

  return std::clamp(sampleCurveY(u), 0.0F, 1.0F);
}

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

  case Easing::CubicBezierDramatic:
    return evaluateCubicBezier(1.0F, 0.0F, 0.0F, 1.0F, t);

  case Easing::CubicBezierAlt:
    return evaluateCubicBezier(0.0F, 0.79F, 1.0F, 0.19F, t);
  }

  return t;
}
