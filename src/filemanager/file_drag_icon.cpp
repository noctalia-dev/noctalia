#include "filemanager/file_drag_icon.h"

#include <cairo.h>
#include <gio/gio.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace filemanager {
  namespace {
    constexpr int kLogicalWidth = 280;
    constexpr int kLogicalHeight = 64;
    constexpr double kCornerCut = 11.0;

    double channel(float value) { return std::clamp(static_cast<double>(value), 0.0, 1.0); }

    void setSource(cairo_t* context, const Color& color, double opacity = 1.0) {
      const double alpha = channel(color.a) * opacity;
      cairo_set_source_rgba(context, channel(color.r), channel(color.g), channel(color.b), alpha);
    }

    void chamferedRectangle(cairo_t* context, double width, double height, double cut) {
      cairo_new_path(context);
      cairo_move_to(context, cut, 0.5);
      cairo_line_to(context, width - cut, 0.5);
      cairo_line_to(context, width - 0.5, cut);
      cairo_line_to(context, width - 0.5, height - cut);
      cairo_line_to(context, width - cut, height - 0.5);
      cairo_line_to(context, cut, height - 0.5);
      cairo_line_to(context, 0.5, height - cut);
      cairo_line_to(context, 0.5, cut);
      cairo_close_path(context);
    }

    void drawFolder(cairo_t* context, double x, double y, const Color& foreground, const Color& accent) {
      cairo_save(context);
      cairo_translate(context, x, y);
      cairo_new_path(context);
      cairo_move_to(context, 1.0, 8.0);
      cairo_line_to(context, 3.0, 4.5);
      cairo_line_to(context, 13.0, 4.5);
      cairo_line_to(context, 17.0, 8.0);
      cairo_line_to(context, 30.0, 8.0);
      cairo_line_to(context, 27.5, 26.0);
      cairo_line_to(context, 1.0, 26.0);
      cairo_close_path(context);
      setSource(context, accent, 0.95);
      cairo_fill_preserve(context);
      setSource(context, foreground, 0.85);
      cairo_set_line_width(context, 1.15);
      cairo_stroke(context);

      cairo_new_path(context);
      cairo_move_to(context, 1.5, 10.0);
      cairo_line_to(context, 29.5, 10.0);
      setSource(context, foreground, 0.48);
      cairo_set_line_width(context, 0.8);
      cairo_stroke(context);
      cairo_restore(context);
    }

    void drawDocument(
        cairo_t* context, double x, double y, const Color& background, const Color& foreground, const Color& accent
    ) {
      cairo_save(context);
      cairo_translate(context, x, y);
      cairo_new_path(context);
      cairo_move_to(context, 5.0, 2.5);
      cairo_line_to(context, 19.5, 2.5);
      cairo_line_to(context, 27.5, 10.0);
      cairo_line_to(context, 27.5, 27.5);
      cairo_line_to(context, 5.0, 27.5);
      cairo_close_path(context);
      setSource(context, background, 0.9);
      cairo_fill_preserve(context);
      setSource(context, accent, 0.95);
      cairo_set_line_width(context, 1.4);
      cairo_stroke(context);

      cairo_new_path(context);
      cairo_move_to(context, 19.5, 3.0);
      cairo_line_to(context, 19.5, 10.5);
      cairo_line_to(context, 27.0, 10.5);
      setSource(context, accent, 0.9);
      cairo_set_line_width(context, 1.2);
      cairo_stroke(context);
      for (int row = 0; row < 3; ++row) {
        const double lineY = 15.0 + static_cast<double>(row) * 4.0;
        cairo_move_to(context, 9.0, lineY);
        cairo_line_to(context, row == 2 ? 19.0 : 23.0, lineY);
      }
      setSource(context, foreground, 0.62);
      cairo_set_line_width(context, 1.0);
      cairo_stroke(context);
      cairo_restore(context);
    }
  } // namespace

  NativeDataTransfer::DragIconImage renderFileDragIcon(
      std::string_view label, bool folder, int scale, Color background, Color foreground, Color accent
  ) {
    const int imageScale = std::clamp(scale, 1, 4);
    const int width = kLogicalWidth * imageScale;
    const int height = kLogicalHeight * imageScale;
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surface);
      return {};
    }

    cairo_t* context = cairo_create(surface);
    cairo_scale(context, imageScale, imageScale);
    chamferedRectangle(context, kLogicalWidth, kLogicalHeight, kCornerCut);
    setSource(context, background);
    cairo_fill_preserve(context);
    setSource(context, accent, 0.78);
    cairo_set_line_width(context, 1.0);
    cairo_stroke(context);

    if (folder)
      drawFolder(context, 17.0, 17.0, foreground, accent);
    else
      drawDocument(context, 17.0, 17.0, background, foreground, accent);

    std::string safeLabel(label);
    std::replace(safeLabel.begin(), safeLabel.end(), '\0', '?');
    gchar* validLabel = g_utf8_make_valid(safeLabel.data(), static_cast<gssize>(safeLabel.size()));
    if (validLabel == nullptr)
      validLabel = g_strdup("(unnamed)");
    if (validLabel[0] == '\0') {
      g_free(validLabel);
      validLabel = g_strdup("(unnamed)");
    }

    PangoLayout* layout = pango_cairo_create_layout(context);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, 208 * PANGO_SCALE);
    pango_layout_set_text(layout, validLabel, -1);
    PangoFontDescription* font = pango_font_description_from_string("Sans 11");
    pango_layout_set_font_description(layout, font);
    setSource(context, foreground);
    cairo_move_to(context, 60.0, 24.0);
    pango_cairo_show_layout(context, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
    g_free(validLabel);

    cairo_surface_flush(surface);
    const auto* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    NativeDataTransfer::DragIconImage image;
    image.width = width;
    image.height = height;
    image.scale = imageScale;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
      std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * width, data + y * stride,
                  static_cast<std::size_t>(width) * sizeof(std::uint32_t));

    cairo_destroy(context);
    cairo_surface_destroy(surface);
    return image;
  }
} // namespace filemanager
