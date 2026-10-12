#pragma once

#include "capture/annotation_document.h"
#include "capture/screencopy_capture.h"

#include <cairo.h>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace capture {

  // Draws one annotation in canvas-logical coordinates; the caller has already applied
  // cairo_scale(scale, scale). `background` is the frozen or captured image as a cairo ARGB32
  // surface in device pixels at the same scale, or nullptr (live mode, where Blur draws nothing).
  void renderAnnotation(cairo_t* cr, const Annotation& annotation, cairo_surface_t* background, double scale);
  void renderAnnotations(
      cairo_t* cr, std::span<const Annotation> annotations, cairo_surface_t* background, double scale,
      std::optional<std::size_t> skipIndex = std::nullopt
  );

  // Logical extents of a Text annotation: x/y are the top-left, the first baseline sits at
  // annotation.points[0].y.
  [[nodiscard]] AnnotationRect measureText(const Annotation& annotation);

  // Frozen straight-alpha RGBA -> cairo ARGB32 (premultiplied, device pixels). Caller destroys.
  [[nodiscard]] cairo_surface_t* frozenToCairo(const ScreencopyImage& image);

  // Background image plus annotations, cropped when the document carries a crop rect.
  // `logicalWidth`/`logicalHeight` describe the canvas the annotation coordinates live in;
  // the result keeps the background's native pixel resolution. Straight RGBA, ready for encodePng.
  [[nodiscard]] ScreencopyImage composeExport(
      const ScreencopyImage& background, double logicalWidth, double logicalHeight, const AnnotationDocument& document
  );

  // Copies a device-pixel rect of a cairo ARGB32 surface into tightly packed straight RGBA.
  // The Image shader premultiplies its texels, so the premultiplication cairo applied is undone here.
  void
  copyArgb32RectToRgba(cairo_surface_t* source, int x, int y, int width, int height, std::vector<std::uint8_t>& out);

  // Releases the single-entry blur cache. Called on overlay teardown so a frozen background
  // surface is not kept alive past the session that produced it.
  void clearBlurCache();

} // namespace capture
