#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/render_styles.h"
#include "ui/controls/gradient.h"
#include "ui/palette.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <print>
#include <thread>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", message);
      return false;
    }
    return true;
  }

  bool nearlyEqual(float a, float b, float epsilon = 1e-5F) { return std::fabs(a - b) <= epsilon; }

} // namespace

int main() {
  bool ok = true;

  // Paint-only contract: gradient state lives in the retained rect style and
  // never touches geometry.
  {
    Gradient gradient;
    gradient.setSize(180.0F, 2.0F);
    gradient.setGradient(
        0.0F,
        {{
            {.position = 0.0F, .color = colorSpecFromRole(ColorRole::Surface)},
            {.position = 0.38F, .color = colorSpecFromRole(ColorRole::Primary, 0.45F)},
            {.position = 0.52F, .color = colorSpecFromRole(ColorRole::Primary)},
            {.position = 1.0F, .color = colorSpecFromRole(ColorRole::Surface)},
        }}
    );
    gradient.setOffset(0.25F);

    const auto& style = gradient.style();
    ok = expect(style.fillMode == FillMode::LinearGradient, "gradient should use linear fill") && ok;
    ok = expect(nearlyEqual(style.gradientAngleDeg, 0.0F), "angle should survive") && ok;
    ok = expect(nearlyEqual(style.gradientOffset, 0.25F), "offset should survive") && ok;
    ok = expect(
             nearlyEqual(gradient.width(), 180.0F) && nearlyEqual(gradient.height(), 2.0F),
             "paint changes should not change geometry"
         )
        && ok;
    ok = expect(gradient.children().size() == 1, "gradient should own exactly one rect node") && ok;
  }

  // Palette re-resolution: role-based stops track palette swaps; fixed stops
  // and the reference default palette stay put.
  {
    const Palette saved = palette;
    Gradient gradient;
    gradient.setGradient(
        0.0F,
        {{
            {.position = 0.0F, .color = colorSpecFromRole(ColorRole::Surface)},
            {.position = 0.38F, .color = colorSpecFromRole(ColorRole::Primary, 0.45F)},
            {.position = 0.52F, .color = colorSpecFromRole(ColorRole::Primary)},
            {.position = 1.0F, .color = fixedColorSpec(rgba(1.0F, 0.0F, 0.0F, 1.0F))},
        }}
    );

    const Color oldPrimary = palette.primary;
    const Color oldStop1 = gradient.style().gradientStops[1].color;
    Palette changed = saved;
    changed.primary = rgba(0.9F, 0.3F, 0.1F, 1.0F);
    setPalette(changed);

    const auto& style = gradient.style();
    ok = expect(
             !(style.gradientStops[2].color.r == oldPrimary.r
               && style.gradientStops[2].color.g == oldPrimary.g
               && style.gradientStops[2].color.b == oldPrimary.b),
             "a role-based stop should resolve the new palette colour"
         )
        && ok;
    ok = expect(
             nearlyEqual(style.gradientStops[1].color.a, changed.primary.a * 0.45F, 1e-3F),
             "a role stop should keep its alpha multiplier"
         )
        && ok;
    ok = expect(
             nearlyEqual(style.gradientStops[3].color.r, 1.0F) && nearlyEqual(style.gradientStops[3].color.g, 0.0F),
             "a fixed stop must not follow the palette"
         )
        && ok;
    (void)oldStop1;

    setPalette(saved);
    const auto& restored = gradient.style();
    ok = expect(
             nearlyEqual(restored.gradientStops[2].color.r, oldPrimary.r)
                 && nearlyEqual(restored.gradientStops[2].color.g, oldPrimary.g)
                 && nearlyEqual(restored.gradientStops[2].color.b, oldPrimary.b),
             "restoring the palette should restore role-resolved stops"
         )
        && ok;
    (void)changed;
  }

  // Angle → axis mapping: the projected rectangle corners must span exactly
  // 0..1 for every angle, so stop positions are stable. Off-axis and swap-dims
  // regressions show up as a range wider than 1 (e.g. 1.414 for an unnormalized
  // diagonal).
  for (const auto& [angle, expectedX, expectedY] : {
           std::tuple{0.0F, 1.0F, 0.0F},
           std::tuple{90.0F, 0.0F, 1.0F},
           std::tuple{45.0F, 0.5F, 0.5F},
           std::tuple{135.0F, -0.5F, 0.5F},
       }) {
    const GradientAxis axis = gradientAxisForDegrees(angle);
    ok = expect(
             nearlyEqual(axis.x, expectedX, 1e-4F) && nearlyEqual(axis.y, expectedY, 1e-4F),
             "axis direction should be normalized"
         )
        && ok;

    float minProj = std::numeric_limits<float>::max();
    float maxProj = std::numeric_limits<float>::lowest();
    for (const auto& corner :
         {std::pair{0.0F, 0.0F}, std::pair{1.0F, 0.0F}, std::pair{0.0F, 1.0F}, std::pair{1.0F, 1.0F}}) {
      const float projection = corner.first * axis.x + corner.second * axis.y - axis.bias;
      minProj = std::min(minProj, projection);
      maxProj = std::max(maxProj, projection);
    }
    ok = expect(nearlyEqual(minProj, 0.0F, 1e-4F), "biased corner projection minimum should be 0") && ok;
    ok = expect(nearlyEqual(maxProj, 1.0F, 1e-4F), "biased corner projection maximum should be 1") && ok;
  }

  // Large angles fold onto the same axis instead of drifting; 0 and 360 agree.
  {
    const GradientAxis a = gradientAxisForDegrees(0.0F);
    const GradientAxis b = gradientAxisForDegrees(360.0F);
    ok = expect(
             nearlyEqual(a.x, b.x) && nearlyEqual(a.y, b.y) && nearlyEqual(a.bias, b.bias),
             "360 degrees should reduce to 0"
         )
        && ok;
  }

  // Native motion: ping-pong runs an outward trip, then chains a return trip.
  // Trips are >= 30 ms with >= 30 ms of slack because AnimationManager measures
  // wall time; assertions stay in broad ranges for the same reason.
  {
    AnimationManager manager;
    Gradient gradient;
    gradient.setMotion(GradientMotion::PingPong, 30.0F, -0.45F, 0.45F);
    gradient.setAnimationManager(&manager);
    ok = expect(manager.hasActive(), "ping-pong should register native motion") && ok;

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    manager.tick(0.0F);
    ok = expect(manager.hasActive(), "ping-pong should start its return trip") && ok;
    ok = expect(gradient.offset() > 0.40F, "ping-pong should reach the far endpoint") && ok;
  }

  // Loop runs the same trip over and over, wrapping back to the start point.
  {
    AnimationManager manager;
    Gradient gradient;
    gradient.setMotion(GradientMotion::Loop, 30.0F, -0.45F, 0.45F);
    gradient.setAnimationManager(&manager);
    ok = expect(manager.hasActive(), "loop should register native motion") && ok;

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    manager.tick(0.0F);
    ok = expect(manager.hasActive(), "loop should restart after a completed trip") && ok;
    ok = expect(gradient.offset() > 0.40F, "loop should reach the endpoint before wrapping") && ok;
    manager.tick(0.0F);
    ok = expect(gradient.offset() < -0.30F, "loop should wrap back to its start point") && ok;
  }

  // Static configurations never register an animation.
  {
    AnimationManager manager;
    Gradient gradient;
    gradient.setAnimationManager(&manager);
    ok = expect(!manager.hasActive(), "no motion means no animation") && ok;

    gradient.setMotion(GradientMotion::None, 30.0F, -0.45F, 0.45F);
    ok = expect(!manager.hasActive(), "explicit none motion stays static") && ok;

    gradient.setMotion(GradientMotion::PingPong, 30.0F, 0.2F, 0.2F);
    ok = expect(!manager.hasActive(), "equal endpoints are static") && ok;
    ok = expect(nearlyEqual(gradient.offset(), 0.2F), "static endpoints pin the offset") && ok;
  }

  // An unchanged motion configuration keeps the running trip and its phase.
  {
    AnimationManager manager;
    Gradient gradient;
    gradient.setMotion(GradientMotion::Loop, 90.0F, -0.45F, 0.45F);
    gradient.setAnimationManager(&manager);
    std::this_thread::sleep_for(std::chrono::milliseconds(45));     // mid-trip: offset ≈ 0
    gradient.setMotion(GradientMotion::Loop, 90.0F, -0.45F, 0.45F); // unchanged
    manager.tick(0.0F);
    ok = expect(gradient.offset() > -0.20F, "an unchanged motion config keeps the current phase") && ok;
  }

  // A changed configuration cancels the old trip and starts one replacement.
  {
    AnimationManager manager;
    Gradient gradient;
    gradient.setMotion(GradientMotion::PingPong, 90.0F, -0.45F, 0.45F);
    gradient.setAnimationManager(&manager);
    std::this_thread::sleep_for(std::chrono::milliseconds(40)); // mid-trip: offset ≈ 0
    gradient.setMotion(GradientMotion::PingPong, 90.0F, 0.3F, 0.45F);
    manager.tick(0.0F);
    ok = expect(manager.hasActive(), "a changed motion config starts a replacement trip") && ok;
    ok = expect(gradient.offset() > 0.20F, "a changed motion config restarts from the new start point") && ok;
  }

  // Destroying the control leaves no owner-bound animation behind.
  {
    AnimationManager manager;
    {
      Gradient gradient;
      gradient.setMotion(GradientMotion::PingPong, 30.0F, -0.45F, 0.45F);
      gradient.setAnimationManager(&manager);
      ok = expect(manager.hasActive(), "bound gradient registers a trip") && ok;
    }
    ok = expect(!manager.hasActive(), "destroying the control cancels its owner-bound animation") && ok;
  }

  // The global speed setting scales a trip when it starts: a trip begun at
  // speed 4 finishes four times faster than one begun at speed 1. This pins
  // the use of animate() rather than animateTimer().
  {
    AnimationManager manager;
    Gradient fast;
    Gradient slow;
    MotionService& motion = MotionService::instance();

    motion.setSpeed(4.0F);
    fast.setMotion(GradientMotion::PingPong, 120.0F, -0.45F, 0.45F);
    fast.setAnimationManager(&manager);

    motion.setSpeed(1.0F);
    slow.setMotion(GradientMotion::PingPong, 120.0F, -0.45F, 0.45F);
    slow.setAnimationManager(&manager);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    manager.tick(0.0F);
    ok = expect(fast.offset() > 0.40F, "a speed-4 trip completes in a quarter of the time") && ok;
    ok = expect(
             slow.offset() > -0.44F && slow.offset() < 0.44F,
             "a speed-1 trip is still mid-flight when the speed-4 trip is done"
         )
        && ok;
    motion.setSpeed(1.0F);
  }

  if (!ok) {
    return 1;
  }
  std::println("ok");
  return 0;
}
