#include "capture/annotation_raster.h"

#include <cairo.h>
#include <cstdint>
#include <cstdio>
#include <print>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "annotation_raster_test: {}", message);
      return false;
    }
    return true;
  }

  std::uint32_t pixelAt(cairo_surface_t* surface, int x, int y) {
    cairo_surface_flush(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    return *reinterpret_cast<const std::uint32_t*>(data + (static_cast<std::size_t>(y) * stride) + (x * 4));
  }

  // Two vertical bands, so a blur that runs has something to average across.
  ScreencopyImage twoToneImage(int width, int height) {
    ScreencopyImage image{
        .width = width,
        .height = height,
        .yInvert = false,
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4U, 0),
    };
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t offset = ((static_cast<std::size_t>(y) * width) + x) * 4U;
        const std::uint8_t value = (x / 4) % 2 == 0 ? 0 : 255;
        image.rgba[offset + 0] = value;
        image.rgba[offset + 1] = value;
        image.rgba[offset + 2] = value;
        image.rgba[offset + 3] = 255;
      }
    }
    return image;
  }

} // namespace

int main() {
  bool ok = true;

  {
    // A brush stroke paints along its path and nowhere else.
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 64);
    cairo_t* cr = cairo_create(surface);
    const capture::Annotation stroke{
        .tool = capture::AnnotationTool::Brush,
        .width = 6.0,
        .stroke = {.r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
        .fill = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F},
        .points = {{.x = 8.0, .y = 32.0}, {.x = 32.0, .y = 32.0}, {.x = 56.0, .y = 32.0}},
        .text = {},
    };
    capture::renderAnnotation(cr, stroke, nullptr, 1.0);
    cairo_destroy(cr);

    const std::uint32_t onPath = pixelAt(surface, 32, 32);
    const std::uint32_t corner = pixelAt(surface, 1, 1);
    ok = expect((onPath >> 24) == 0xFFU, "a stroked pixel is opaque") && ok;
    ok = expect(((onPath >> 16) & 0xFFU) == 0xFFU, "the stroke keeps its red channel") && ok;
    ok = expect((onPath & 0xFFFFU) == 0U, "the stroke has no green or blue") && ok;
    ok = expect(corner == 0U, "a pixel away from the path stays transparent") && ok;
    cairo_surface_destroy(surface);
  }

  {
    // Blur rewrites only the pixels inside its rect.
    const ScreencopyImage background = twoToneImage(64, 64);
    cairo_surface_t* source = capture::frozenToCairo(background);
    ok = expect(source != nullptr, "frozenToCairo produces a surface") && ok;
    if (source == nullptr) {
      return 1;
    }
    cairo_surface_t* target = capture::frozenToCairo(background);

    const std::uint32_t insideBefore = pixelAt(target, 24, 24);
    const std::uint32_t outsideBefore = pixelAt(target, 45, 24);

    cairo_t* cr = cairo_create(target);
    const capture::Annotation blur{
        .tool = capture::AnnotationTool::Blur,
        .width = 32.0,
        .stroke = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
        .fill = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F},
        .points = {{.x = 10.0, .y = 10.0}, {.x = 40.0, .y = 40.0}},
        .text = {},
    };
    capture::renderAnnotation(cr, blur, source, 1.0);
    cairo_destroy(cr);

    ok = expect(pixelAt(target, 24, 24) != insideBefore, "blur changes pixels inside its rect") && ok;
    ok = expect(pixelAt(target, 45, 24) == outsideBefore, "blur leaves a pixel 5 px outside its rect alone") && ok;

    capture::clearBlurCache();
    cairo_surface_destroy(target);
    cairo_surface_destroy(source);
  }

  {
    // Export keeps the capture's native resolution: a logical crop scales by the image/logical ratio.
    const ScreencopyImage background = twoToneImage(200, 200);
    capture::AnnotationDocument doc;
    doc.setCrop(capture::AnnotationRect{.x = 10.0, .y = 10.0, .width = 20.0, .height = 20.0});
    const ScreencopyImage exported = capture::composeExport(background, 100.0, 100.0, doc);
    ok = expect(exported.width == 40 && exported.height == 40, "composeExport scales the crop to device pixels") && ok;
    ok = expect(exported.rgba.size() == static_cast<std::size_t>(40) * 40U * 4U, "the exported buffer matches its size")
        && ok;
  }

  {
    // Without a crop the export covers the whole capture and carries the ink.
    const ScreencopyImage background = twoToneImage(64, 64);
    capture::AnnotationDocument doc;
    doc.push(
        capture::Annotation{
            .tool = capture::AnnotationTool::Rectangle,
            .width = 4.0,
            .stroke = {.r = 0.0F, .g = 1.0F, .b = 0.0F, .a = 1.0F},
            .fill = {.r = 0.0F, .g = 1.0F, .b = 0.0F, .a = 1.0F},
            .points = {{.x = 8.0, .y = 8.0}, {.x = 56.0, .y = 56.0}},
            .text = {},
        }
    );
    const ScreencopyImage exported = capture::composeExport(background, 64.0, 64.0, doc);
    ok = expect(exported.width == 64 && exported.height == 64, "an uncropped export keeps the capture size") && ok;

    const std::size_t center = ((static_cast<std::size_t>(32) * 64U) + 32U) * 4U;
    ok = expect(
             exported.rgba[center + 0] == 0 && exported.rgba[center + 1] == 255 && exported.rgba[center + 2] == 0,
             "the filled rectangle reaches the exported pixels"
         )
        && ok;
    ok = expect(exported.rgba[center + 3] == 255, "the exported image is opaque") && ok;
  }

  {
    // Text measures to a box whose baseline sits at the anchor's y.
    const capture::Annotation text{
        .tool = capture::AnnotationTool::Text,
        .width = 24.0,
        .stroke = {.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F},
        .fill = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F},
        .points = {{.x = 30.0, .y = 40.0}},
        .text = "Hello",
    };
    const capture::AnnotationRect box = capture::measureText(text);
    ok = expect(box.width > 0.0 && box.height > 0.0, "measureText reports non-empty extents") && ok;
    ok = expect(box.x == 30.0, "the measured box starts at the anchor's x") && ok;
    ok = expect(box.y < 40.0 && box.y + box.height > 40.0, "the anchor's baseline falls inside the measured box") && ok;

    const capture::Annotation empty{
        .tool = capture::AnnotationTool::Text,
        .width = 24.0,
        .stroke = text.stroke,
        .fill = text.fill,
        .points = {{.x = 30.0, .y = 40.0}},
        .text = {},
    };
    const capture::AnnotationRect emptyBox = capture::measureText(empty);
    ok = expect(emptyBox.height > 0.0, "empty text still reports a line height so the caret has a size") && ok;
  }

  {
    // Layer uploads must hand the renderer straight alpha; cairo stores premultiplied.
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.5);
    cairo_paint(cr);
    cairo_destroy(cr);

    std::vector<std::uint8_t> out;
    capture::copyArgb32RectToRgba(surface, 0, 0, 4, 4, out);
    ok = expect(out.size() == 4U * 4U * 4U, "copyArgb32RectToRgba fills the requested rect") && ok;
    ok = expect(out[0] >= 254, "a half-transparent red un-premultiplies back to full red") && ok;
    ok = expect(out[3] >= 127 && out[3] <= 128, "the alpha channel is preserved") && ok;

    std::vector<std::uint8_t> outOfBounds;
    capture::copyArgb32RectToRgba(surface, 2, 2, 4, 4, outOfBounds);
    ok = expect(outOfBounds.empty(), "a rect past the surface edge yields nothing") && ok;
    cairo_surface_destroy(surface);
  }

  return ok ? 0 : 1;
}
