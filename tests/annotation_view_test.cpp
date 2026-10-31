#include "capture/annotation_view.h"
#include "tests/test_check.h"

#include <cmath>
#include <print>

namespace {

  bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

  const capture::AnnotationRect kFitted{.x = 100.0, .y = 200.0, .width = 800.0, .height = 600.0};

} // namespace

int main() {
  // Zoom is clamped to the agreed 0.5x-16x range.
  TEST_CHECK(near(capture::clampViewZoom(0.1), capture::kMinViewZoom));
  TEST_CHECK(near(capture::clampViewZoom(100.0), capture::kMaxViewZoom));
  TEST_CHECK(near(capture::clampViewZoom(2.5), 2.5));
  TEST_CHECK(near(capture::clampViewZoom(1.0), 1.0));

  // Identity view reproduces the unzoomed fitted layout exactly.
  const capture::ViewTransform view{};
  const capture::AnnotationRect rect = capture::viewRect(view, kFitted);
  TEST_CHECK(rect.x == kFitted.x);
  TEST_CHECK(rect.y == kFitted.y);
  TEST_CHECK(rect.width == kFitted.width);
  TEST_CHECK(rect.height == kFitted.height);
  TEST_CHECK(view.isIdentity());

  const capture::AnnotationPoint surface{.x = 350.0, .y = 450.0};
  const capture::AnnotationPoint canvas = capture::surfaceToCanvas(view, kFitted, surface);
  TEST_CHECK(near(canvas.x, 250.0));
  TEST_CHECK(near(canvas.y, 250.0));
  const capture::AnnotationPoint back = capture::canvasToSurface(view, kFitted, canvas);
  TEST_CHECK(near(back.x, surface.x));
  TEST_CHECK(near(back.y, surface.y));

  // Mapping roundtrips under an arbitrary zoom + pan.
  const capture::ViewTransform arbitrary{.zoom = 3.25, .panX = -40.0, .panY = 17.5};
  const capture::AnnotationPoint surface2{.x = 1234.5, .y = 678.25};
  const capture::AnnotationPoint canvas2 = capture::surfaceToCanvas(arbitrary, kFitted, surface2);
  const capture::AnnotationPoint back2 = capture::canvasToSurface(arbitrary, kFitted, canvas2);
  TEST_CHECK(near(back2.x, surface2.x));
  TEST_CHECK(near(back2.y, surface2.y));

  // Pointer-anchored zoom keeps the canvas point under the anchor fixed.
  const double anchorX = 500.0;
  const double anchorY = 400.0;
  const capture::AnnotationPoint before = capture::surfaceToCanvas(view, kFitted, {.x = anchorX, .y = anchorY});
  const capture::ViewTransform zoomed = capture::zoomViewAt(view, kFitted, anchorX, anchorY, 2.0);
  TEST_CHECK(near(zoomed.zoom, 2.0));
  const capture::AnnotationPoint after = capture::surfaceToCanvas(zoomed, kFitted, {.x = anchorX, .y = anchorY});
  TEST_CHECK(near(after.x, before.x));
  TEST_CHECK(near(after.y, before.y));

  // Anchored zoom clamps at both range ends, and the anchor invariant holds at the clamped zoom.
  const capture::ViewTransform huge = capture::zoomViewAt(view, kFitted, anchorX, anchorY, 1000.0);
  TEST_CHECK(near(huge.zoom, capture::kMaxViewZoom));
  const capture::ViewTransform tiny = capture::zoomViewAt(view, kFitted, anchorX, anchorY, 0.001);
  TEST_CHECK(near(tiny.zoom, capture::kMinViewZoom));
  const capture::AnnotationPoint afterClamp = capture::surfaceToCanvas(huge, kFitted, {.x = anchorX, .y = anchorY});
  TEST_CHECK(near(afterClamp.x, before.x));
  TEST_CHECK(near(afterClamp.y, before.y));

  // Pan clamp keeps at least kViewPanMinVisible of the canvas inside the viewport.
  const double viewportW = 1920.0;
  const double viewportH = 1080.0;
  TEST_CHECK(capture::clampViewPan(view, kFitted, viewportW, viewportH).isIdentity());

  const capture::ViewTransform wild{.zoom = 1.0, .panX = 1e6, .panY = -1e6};
  const capture::ViewTransform clamped = capture::clampViewPan(wild, kFitted, viewportW, viewportH);
  const capture::AnnotationRect clampedRect = capture::viewRect(clamped, kFitted);
  TEST_CHECK(clampedRect.x <= viewportW - capture::kViewPanMinVisible);
  TEST_CHECK(clampedRect.x + clampedRect.width >= capture::kViewPanMinVisible);
  TEST_CHECK(clampedRect.y <= viewportH - capture::kViewPanMinVisible);
  TEST_CHECK(clampedRect.y + clampedRect.height >= capture::kViewPanMinVisible);

  // Zoomed-in pans clamp on the same rule: an oversized canvas keeps a visible slice.
  const capture::ViewTransform zoomedWild{.zoom = 8.0, .panX = 5000.0, .panY = -5000.0};
  const capture::ViewTransform zoomedClamped = capture::clampViewPan(zoomedWild, kFitted, 1280.0, 720.0);
  const capture::AnnotationRect zoomedRect = capture::viewRect(zoomedClamped, kFitted);
  TEST_CHECK(zoomedRect.x <= 1280.0 - capture::kViewPanMinVisible);
  TEST_CHECK(zoomedRect.x + zoomedRect.width >= capture::kViewPanMinVisible);
  TEST_CHECK(zoomedRect.y <= 720.0 - capture::kViewPanMinVisible);
  TEST_CHECK(zoomedRect.y + zoomedRect.height >= capture::kViewPanMinVisible);
  TEST_CHECK(near(zoomedClamped.zoom, 8.0));

  // viewRect scales the fitted size by the zoom and offsets by the pan.
  const capture::ViewTransform scaled{.zoom = 2.5, .panX = 10.0, .panY = -10.0};
  const capture::AnnotationRect scaledRect = capture::viewRect(scaled, kFitted);
  TEST_CHECK(near(scaledRect.width, kFitted.width * 2.5));
  TEST_CHECK(near(scaledRect.height, kFitted.height * 2.5));
  TEST_CHECK(near(scaledRect.x, kFitted.x + 10.0));
  TEST_CHECK(near(scaledRect.y, kFitted.y - 10.0));

  std::println("annotation_view_test: all checks passed");
  return 0;
}
