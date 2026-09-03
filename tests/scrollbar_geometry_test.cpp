#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/scrollbar.h"
#include "ui/style.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <print>
#include <string_view>
#include <thread>
#include <vector>

namespace {

  int gFailures = 0;

  void expectNear(float actual, float expected, std::string_view what) {
    if (std::abs(actual - expected) >= 0.01F) {
      std::println(stderr, "scrollbar_geometry_test: {} is {}, expected {}", what, actual, expected);
      ++gFailures;
    }
  }

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view, float fontSize, FontWeight, float, int, TextAlign, std::string_view, TextEllipsize, bool
    ) override {
      return TextMetrics{.bottom = fontSize};
    }

    TextMetrics measureFont(float fontSize, FontWeight) override { return TextMetrics{.bottom = fontSize}; }

    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, FontWeight
    ) override {}

    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, FontWeight
    ) override {}

    TextMetrics measureGlyph(char32_t, float) override { return TextMetrics{}; }

    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  // Scrollbar composes its children in this order.
  constexpr std::size_t kTrackIndex = 0;
  constexpr std::size_t kThumbIndex = 1;
  constexpr std::size_t kTrackAreaIndex = 2;
  constexpr std::size_t kThumbAreaIndex = 3;

  constexpr float kViewWidth = 200.0F;

  struct Harness {
    std::unique_ptr<ScrollView> view;
    Node* track = nullptr;
    Node* thumb = nullptr;
    InputArea* trackArea = nullptr;
    InputArea* thumbArea = nullptr;
  };

  Harness layoutScrollView(
      Renderer& renderer, AnimationManager& animations, float contentScale, float viewportHeight, float contentHeight
  ) {
    Harness harness;
    harness.view = std::make_unique<ScrollView>();
    harness.view->setAnimationManager(&animations);
    harness.view->setContentScale(contentScale);
    harness.view->setViewportPaddingH(0.0F);
    harness.view->setViewportPaddingV(0.0F);

    auto content = std::make_unique<Box>();
    content->setSize(10.0F, contentHeight);
    harness.view->content()->addChild(std::move(content));

    harness.view->setSize(kViewWidth, viewportHeight);
    harness.view->layout(renderer);

    Node* bar = nullptr;
    for (const auto& child : harness.view->children()) {
      if (dynamic_cast<Scrollbar*>(child.get()) != nullptr) {
        bar = child.get();
      }
    }
    if (bar == nullptr || bar->children().size() <= kThumbAreaIndex) {
      std::println(stderr, "scrollbar_geometry_test: scrollbar children missing");
      std::exit(1);
    }
    harness.track = bar->children()[kTrackIndex].get();
    harness.thumb = bar->children()[kThumbIndex].get();
    harness.trackArea = dynamic_cast<InputArea*>(bar->children()[kTrackAreaIndex].get());
    harness.thumbArea = dynamic_cast<InputArea*>(bar->children()[kThumbAreaIndex].get());
    return harness;
  }

  // AnimationManager advances from wall time, so run it until the queue drains.
  void settle(AnimationManager& animations) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    animations.tick(0.0F);
    while (animations.hasActive() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      animations.tick(0.0F);
    }
  }

} // namespace

int main() {
  StubRenderer renderer;
  AnimationManager animations;

  // Base scale: the reserved gutter and bar thickness are the historical logical sizes.
  {
    auto harness = layoutScrollView(renderer, animations, 1.0F, 100.0F, 400.0F);
    expectNear(harness.view->scrollbarGutter(), Style::scrollbarWidth + Style::scrollbarGap, "gutter at scale 1");
    expectNear(harness.track->width(), Style::scrollbarWidth, "track thickness at scale 1");
    expectNear(harness.track->x(), 0.0F, "track offset at rest");
    expectNear(
        harness.view->contentViewportWidth(), kViewWidth - (Style::scrollbarWidth + Style::scrollbarGap),
        "content width at scale 1"
    );
    expectNear(harness.trackArea->hitTestOutset().left, Style::scrollbarHitSlop, "track hit slop toward content");
    expectNear(harness.thumbArea->hitTestOutset().left, Style::scrollbarHitSlop, "thumb hit slop toward content");
    expectNear(harness.thumbArea->hitTestOutset().right, 0.0F, "no hit slop past the outer edge");
  }

  // The whole scrollbar family tracks the host surface scale, like every other Style constant.
  {
    auto harness = layoutScrollView(renderer, animations, 2.0F, 100.0F, 400.0F);
    expectNear(
        harness.view->scrollbarGutter(), (Style::scrollbarWidth + Style::scrollbarGap) * 2.0F, "gutter at scale 2"
    );
    expectNear(harness.track->width(), Style::scrollbarWidth * 2.0F, "track thickness at scale 2");
    expectNear(harness.thumb->height(), Style::scrollbarMinThumbHeight * 2.0F, "thumb floor at scale 2");
    expectNear(harness.trackArea->hitTestOutset().left, Style::scrollbarHitSlop * 2.0F, "hit slop at scale 2");
  }

  // Hovering expands the bar over the content: the reserved gutter, and therefore the content
  // width, must not move, or the view would reflow under the pointer.
  {
    auto harness = layoutScrollView(renderer, animations, 1.0F, 100.0F, 400.0F);
    const float gutter = harness.view->scrollbarGutter();
    const float contentWidth = harness.view->contentViewportWidth();

    harness.trackArea->dispatchEnter(1.0F, 1.0F);
    settle(animations);
    expectNear(harness.track->width(), Style::scrollbarHoverWidth, "hovered track thickness");
    expectNear(harness.thumb->width(), Style::scrollbarHoverWidth, "hovered thumb thickness");
    const float overlay = -(Style::scrollbarHoverWidth - Style::scrollbarWidth);
    expectNear(harness.track->x(), overlay, "hovered track grows inward");
    // The thumb must follow the track inward, or its outer half lands outside the gutter and the
    // scroll view clips it: the bar looks expanded while the thumb stays thin.
    expectNear(harness.thumb->x(), overlay, "hovered thumb grows inward");
    expectNear(harness.thumbArea->x(), overlay, "hovered thumb hit area grows inward");
    expectNear(harness.view->scrollbarGutter(), gutter, "gutter while hovered");
    expectNear(harness.view->contentViewportWidth(), contentWidth, "content width while hovered");

    harness.trackArea->dispatchLeave();
    settle(animations);
    expectNear(harness.track->width(), Style::scrollbarWidth, "thickness after leave");
    expectNear(harness.track->x(), 0.0F, "offset after leave");
    expectNear(harness.thumb->x(), 0.0F, "thumb offset after leave");
  }

  // Entering the bar only queues an animation; it mutates no node. Unless it also marks paint
  // dirty, the surface never schedules a frame, so the animation is not ticked until some
  // unrelated event repaints and the expansion snaps to its end value.
  {
    auto harness = layoutScrollView(renderer, animations, 1.0F, 100.0F, 400.0F);
    harness.view->clearDirty();
    if (harness.view->paintDirty()) {
      std::println(stderr, "scrollbar_geometry_test: clearDirty() left the tree dirty");
      ++gFailures;
    }
    harness.trackArea->dispatchEnter(1.0F, 1.0F);
    if (!harness.view->paintDirty()) {
      std::println(stderr, "scrollbar_geometry_test: hover did not request a repaint");
      ++gFailures;
    }
    settle(animations);
  }

  // A thumb thinner than the bar is thick would render as a blob.
  {
    auto harness = layoutScrollView(renderer, animations, 1.0F, 100.0F, 100000.0F);
    expectNear(harness.thumb->height(), Style::scrollbarMinThumbHeight, "thumb floor with tiny ratio");
    harness.thumbArea->dispatchEnter(1.0F, 1.0F);
    settle(animations);
    const bool floorHoldsHovered = harness.thumb->height() >= Style::scrollbarHoverWidth;
    if (!floorHoldsHovered) {
      std::println(stderr, "scrollbar_geometry_test: hovered thumb is thicker than it is long");
      ++gFailures;
    }
  }

  if (gFailures != 0) {
    std::println(stderr, "scrollbar_geometry_test: {} failure(s)", gFailures);
    return 1;
  }
  std::println("scrollbar_geometry_test: ok");
  return 0;
}
