#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/render_styles.h"
#include "ui/controls/label.h"
#include "ui/palette.h"

#include <array>
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

  // The Animoo crest: dim shoulders around one bright band.
  std::array<GradientColorStop, 4> crestStops() {
    return {{
        {.position = 0.40F, .color = colorSpecFromRole(ColorRole::Primary, 0.08F)},
        {.position = 0.50F, .color = fixedColorSpec(Color{0.96F, 1.0F, 1.0F, 1.0F})},
        {.position = 0.60F, .color = colorSpecFromRole(ColorRole::Primary, 0.08F)},
        {.position = 0.60F, .color = colorSpecFromRole(ColorRole::Primary, 0.08F)},
    }};
  }

} // namespace

int main() {
  bool ok = true;

  // Paint-only contract: a gradient rides on the text node's style and the
  // label keeps its solid colour as the fallback.
  {
    Label label;
    label.setText("アニムー");
    label.setColor(colorSpecFromRole(ColorRole::OnSurface));
    ok = expect(!label.gradientStyle().enabled, "a fresh label has no gradient") && ok;

    label.setGradient(0.0F, crestStops());
    label.setGradientGlowRadius(3.0F);
    label.setGradientOffset(0.25F);

    const auto& style = label.gradientStyle();
    ok = expect(style.enabled, "setGradient enables the text gradient") && ok;
    ok = expect(nearlyEqual(style.angleDeg, 0.0F), "angle survives") && ok;
    ok = expect(nearlyEqual(style.offset, 0.25F), "offset survives") && ok;
    ok = expect(nearlyEqual(style.glowRadius, 3.0F), "glow radius survives") && ok;
    ok = expect(nearlyEqual(style.stops[1].position, 0.50F), "the crest stop survives") && ok;
    ok = expect(label.color().a > 0.0F, "the solid fallback colour is untouched") && ok;

    label.clearGradient();
    ok = expect(!label.gradientStyle().enabled, "clearGradient disables the text gradient") && ok;
    ok = expect(nearlyEqual(label.gradientStyle().glowRadius, 0.0F), "clearGradient drops the halo too") && ok;
  }

  // Role stops re-resolve on a palette swap; fixed stops do not move.
  {
    Label label;
    label.setText("アニムー");
    label.setGradient(0.0F, crestStops());
    const Color beforeRole = label.gradientStyle().stops[0].color;
    const Color beforeFixed = label.gradientStyle().stops[1].color;

    const Palette saved = palette;
    Palette swapped = saved;
    swapped.primary = Color{0.1F, 0.9F, 0.4F, 1.0F};
    setPalette(swapped);

    const Color afterRole = label.gradientStyle().stops[0].color;
    const Color afterFixed = label.gradientStyle().stops[1].color;
    ok = expect(!(afterRole == beforeRole), "a role stop follows the palette") && ok;
    ok = expect(afterFixed == beforeFixed, "a fixed stop ignores the palette") && ok;
    ok = expect(nearlyEqual(afterRole.a, 0.08F), "the role stop keeps its declared alpha") && ok;
    setPalette(saved);
  }

  // A moving offset is paint, not layout: the measured text must not budge.
  {
    Label label;
    label.setText("アニムー");
    label.setGradient(0.0F, crestStops());
    label.setSize(120.0F, 20.0F);
    const float width = label.width();
    const float height = label.height();

    for (float offset = -0.6F; offset <= 0.6F; offset += 0.2F) {
      label.setGradientOffset(offset);
    }
    ok = expect(
             nearlyEqual(label.width(), width) && nearlyEqual(label.height(), height),
             "animating the offset never changes measured geometry"
         )
        && ok;
    ok = expect(label.gradientStyle().enabled, "the gradient survives an offset sweep") && ok;
  }

  // Motion lifetime, copied from the Gradient control's rules.
  {
    AnimationManager manager;
    Label label;
    label.setText("アニムー");
    label.setGradient(0.0F, crestStops());
    label.setAnimationManager(&manager);
    ok = expect(!manager.hasActive(), "a label with no motion registers no trip") && ok;

    label.setGradientMotion(GradientMotion::Loop, 40.0F, -0.6F, 0.6F);
    ok = expect(manager.hasActive(), "loop motion registers a trip") && ok;

    // Re-applying the identical configuration must keep the running trip.
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    manager.tick(0.0F);
    const float midFlight = label.gradientOffset();
    label.setGradientMotion(GradientMotion::Loop, 40.0F, -0.6F, 0.6F);
    ok = expect(manager.hasActive(), "an unchanged motion config keeps its trip") && ok;
    ok = expect(nearlyEqual(label.gradientOffset(), midFlight), "an unchanged motion config keeps its phase") && ok;

    // A changed configuration replaces the trip rather than stacking one.
    label.setGradientMotion(GradientMotion::PingPong, 25.0F, -0.3F, 0.3F);
    ok = expect(manager.hasActive(), "a changed motion config still animates") && ok;

    label.setGradientMotionVisible(false);
    ok = expect(!manager.hasActive(), "hiding a label stops its trip") && ok;
    manager.tick(0.0F);
    ok = expect(!manager.hasActive(), "a hidden label stays stopped after a tick") && ok;

    label.setGradientMotionVisible(true);
    ok = expect(manager.hasActive(), "showing a label restarts its trip") && ok;

    label.setGradientMotion(GradientMotion::None, 0.0F, 0.0F, 0.0F);
    ok = expect(!manager.hasActive(), "clearing motion cancels the trip") && ok;
  }

  // Equal endpoints pin the offset instead of animating.
  {
    AnimationManager manager;
    Label label;
    label.setText("アニムー");
    label.setGradient(0.0F, crestStops());
    label.setAnimationManager(&manager);
    label.setGradientMotion(GradientMotion::Loop, 40.0F, 0.2F, 0.2F);
    ok = expect(!manager.hasActive(), "equal endpoints need no trip") && ok;
    ok = expect(nearlyEqual(label.gradientOffset(), 0.2F), "equal endpoints pin the static offset") && ok;
  }

  // A destroyed label cancels its owner-bound trip.
  {
    AnimationManager manager;
    {
      Label label;
      label.setText("アニムー");
      label.setGradient(0.0F, crestStops());
      label.setAnimationManager(&manager);
      label.setGradientMotion(GradientMotion::Loop, 40.0F, -0.6F, 0.6F);
      ok = expect(manager.hasActive(), "the scoped label animates") && ok;
    }
    ok = expect(!manager.hasActive(), "destroying a label cancels its trip") && ok;
    manager.tick(0.0F);
    ok = expect(!manager.hasActive(), "a destroyed label leaves no repeating callback") && ok;
  }

  // Reduced motion parks at the midpoint and never chains a 1 ms completion.
  {
    auto& motion = MotionService::instance();
    motion.setEnabled(true);
    AnimationManager manager;
    Label label;
    label.setText("アニムー");
    label.setGradient(0.0F, crestStops());
    label.setAnimationManager(&manager);
    label.setGradientMotion(GradientMotion::Loop, 40.0F, -0.6F, 0.6F);
    ok = expect(manager.hasActive(), "the label animates before reduced motion") && ok;

    motion.setEnabled(false);
    ok = expect(!manager.hasActive(), "reduced motion cancels the running trip") && ok;
    ok = expect(nearlyEqual(label.gradientOffset(), 0.0F), "reduced motion parks at the midpoint") && ok;

    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    for (int i = 0; i < 3; ++i) {
      manager.tick(0.0F);
    }
    ok = expect(!manager.hasActive(), "reduced motion ticks must not chain completions") && ok;
    ok = expect(nearlyEqual(label.gradientOffset(), 0.0F), "reduced motion stays parked") && ok;

    motion.setEnabled(true);
    ok = expect(manager.hasActive(), "restoring motion resumes the label trip") && ok;
    motion.setEnabled(true);
  }

  if (!ok) {
    return 1;
  }
  std::println("ok");
  return 0;
}
