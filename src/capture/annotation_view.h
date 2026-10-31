#pragma once

#include "capture/annotation_document.h"

#include <algorithm>
#include <utility>

namespace capture {

  // View transform layered on top of the fitted canvas rect computed by layoutCanvas.
  // It only changes where the canvas is displayed and how surface points map to canvas
  // points; annotation geometry, cairo rendering and export all stay in canvas space.

  constexpr double kMinViewZoom = 0.5;
  constexpr double kMaxViewZoom = 16.0;
  // Minimum slice of the zoomed canvas kept inside the viewport while panning
  // (surface-logical pixels), so the canvas can never be dragged fully out of reach.
  constexpr double kViewPanMinVisible = 64.0;

  struct ViewTransform {
    // Multiplier over the fitted canvas size. 1.0 = the unzoomed fitted view.
    double zoom = 1.0;
    // Surface-logical offset of the zoomed canvas origin from the fitted origin.
    double panX = 0.0;
    double panY = 0.0;

    [[nodiscard]] constexpr bool isIdentity() const noexcept { return zoom == 1.0 && panX == 0.0 && panY == 0.0; }
  };

  [[nodiscard]] constexpr double clampViewZoom(double zoom) noexcept {
    return std::clamp(zoom, kMinViewZoom, kMaxViewZoom);
  }

  // Displayed rect of the canvas under the view transform (surface-logical coordinates).
  [[nodiscard]] constexpr AnnotationRect viewRect(const ViewTransform& view, const AnnotationRect& fitted) noexcept {
    return AnnotationRect{
        .x = fitted.x + view.panX,
        .y = fitted.y + view.panY,
        .width = fitted.width * view.zoom,
        .height = fitted.height * view.zoom,
    };
  }

  [[nodiscard]] constexpr AnnotationPoint
  surfaceToCanvas(const ViewTransform& view, const AnnotationRect& fitted, AnnotationPoint surface) noexcept {
    return AnnotationPoint{
        .x = (surface.x - fitted.x - view.panX) / view.zoom,
        .y = (surface.y - fitted.y - view.panY) / view.zoom,
    };
  }

  [[nodiscard]] constexpr AnnotationPoint
  canvasToSurface(const ViewTransform& view, const AnnotationRect& fitted, AnnotationPoint canvas) noexcept {
    return AnnotationPoint{
        .x = fitted.x + view.panX + canvas.x * view.zoom,
        .y = fitted.y + view.panY + canvas.y * view.zoom,
    };
  }

  // Zoom by `factor`, keeping the canvas point under the surface anchor fixed.
  [[nodiscard]] constexpr ViewTransform zoomViewAt(
      const ViewTransform& view, const AnnotationRect& fitted, double anchorX, double anchorY, double factor
  ) noexcept {
    const AnnotationPoint anchor = surfaceToCanvas(view, fitted, {.x = anchorX, .y = anchorY});
    const double zoom = clampViewZoom(view.zoom * factor);
    return ViewTransform{
        .zoom = zoom,
        .panX = anchorX - fitted.x - anchor.x * zoom,
        .panY = anchorY - fitted.y - anchor.y * zoom,
    };
  }

  // Clamp pan so at least kViewPanMinVisible of the zoomed canvas stays inside the
  // viewport. A canvas smaller than the viewport may still move, but never leave it.
  [[nodiscard]] constexpr ViewTransform clampViewPan(
      const ViewTransform& view, const AnnotationRect& fitted, double viewportWidth, double viewportHeight
  ) noexcept {
    const double width = fitted.width * view.zoom;
    const double height = fitted.height * view.zoom;
    const double rectX = fitted.x + view.panX;
    const double rectY = fitted.y + view.panY;

    // rect.x >= minVisible - width keeps the right edge reachable;
    // rect.x <= viewportWidth - minVisible keeps the left edge reachable.
    const double lowX = kViewPanMinVisible - width;
    const double highX = viewportWidth - kViewPanMinVisible;
    const double lowY = kViewPanMinVisible - height;
    const double highY = viewportHeight - kViewPanMinVisible;

    const auto clampAxis = [](double value, double low, double high) noexcept {
      if (low > high) {
        // Degenerate viewport: pin to the interval midpoint instead of clamping UB.
        return (low + high) * 0.5;
      }
      return std::clamp(value, low, high);
    };

    return ViewTransform{
        .zoom = view.zoom,
        .panX = clampAxis(rectX, lowX, highX) - fitted.x,
        .panY = clampAxis(rectY, lowY, highY) - fitted.y,
    };
  }

} // namespace capture
