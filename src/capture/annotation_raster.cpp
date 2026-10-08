#include "capture/annotation_raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <numbers>
#include <pango/pangocairo.h>
#include <string>
#include <utility>

namespace capture {
  namespace {

    constexpr const char* kTextFontFamily = "Sans Bold";
    constexpr double kHighlighterAlpha = 0.45;
    constexpr int kBlurCacheCapacity = 4;
    constexpr int kMaxBlurStep = 16;
    // Ceiling on the pixels the box passes touch, so a full-screen blur rect costs no more
    // than a small one; anything above this is detail the blur discards.
    constexpr std::int64_t kMaxBlurWorkPixels = 256 * 256;

    // A blurred region kept at 1/step resolution; the caller scales it back when painting.
    struct BlurResult {
      cairo_surface_t* surface = nullptr;
      int step = 1;
    };

    struct TextMetrics {
      double left = 0.0; // relative to the anchor x
      double top = 0.0;  // relative to the anchor y (baseline), negative above it
      double width = 0.0;
      double height = 0.0;
    };

    [[nodiscard]] PangoFontDescription* makeTextFont(double sizePx) {
      PangoFontDescription* font = pango_font_description_from_string(kTextFontFamily);
      pango_font_description_set_absolute_size(font, std::max(1.0, sizePx) * PANGO_SCALE);
      return font;
    }

    void applyTextLayout(PangoLayout* layout, const Annotation& annotation) {
      PangoFontDescription* font = makeTextFont(annotation.width);
      pango_layout_set_font_description(layout, font);
      pango_font_description_free(font);
      pango_layout_set_single_paragraph_mode(layout, TRUE);
      pango_layout_set_text(layout, annotation.text.c_str(), -1);
    }

    [[nodiscard]] TextMetrics measureTextUncached(const Annotation& annotation) {
      cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
      cairo_t* cr = cairo_create(surface);
      PangoLayout* layout = pango_cairo_create_layout(cr);
      applyTextLayout(layout, annotation);

      int layoutWidth = 0;
      int layoutHeight = 0;
      pango_layout_get_pixel_size(layout, &layoutWidth, &layoutHeight);
      const double baseline = static_cast<double>(pango_layout_get_baseline(layout)) / PANGO_SCALE;

      g_object_unref(layout);
      cairo_destroy(cr);
      cairo_surface_destroy(surface);

      return TextMetrics{
          .left = 0.0,
          .top = -baseline,
          .width = static_cast<double>(layoutWidth),
          .height = static_cast<double>(layoutHeight),
      };
    }

    [[nodiscard]] TextMetrics textMetrics(const Annotation& annotation) {
      // Pango measurement dominates hit testing and dirty-rect math during a text edit,
      // so results are memoized per (content, font size).
      static std::map<std::pair<std::string, double>, TextMetrics> cache;
      const auto key = std::pair<std::string, double>(annotation.text, annotation.width);
      const auto it = cache.find(key);
      if (it != cache.end()) {
        return it->second;
      }
      if (cache.size() > 512) {
        cache.clear();
      }
      const TextMetrics metrics = measureTextUncached(annotation);
      cache.emplace(key, metrics);
      return metrics;
    }

    struct BlurCacheEntry {
      cairo_surface_t* surface = nullptr;
      const cairo_surface_t* source = nullptr;
      int x = 0;
      int y = 0;
      int width = 0;
      int height = 0;
      int step = 1;
      double strength = 0.0;
      std::uint64_t stamp = 0;
    };

    std::array<BlurCacheEntry, kBlurCacheCapacity>& blurCache() {
      static std::array<BlurCacheEntry, kBlurCacheCapacity> cache{};
      return cache;
    }

    void addPixel(std::uint32_t pixel, std::array<std::uint32_t, 4>& sums) {
      sums[0] += pixel & 0xFFU;
      sums[1] += (pixel >> 8) & 0xFFU;
      sums[2] += (pixel >> 16) & 0xFFU;
      sums[3] += (pixel >> 24) & 0xFFU;
    }

    void subtractPixel(std::uint32_t pixel, std::array<std::uint32_t, 4>& sums) {
      sums[0] -= pixel & 0xFFU;
      sums[1] -= (pixel >> 8) & 0xFFU;
      sums[2] -= (pixel >> 16) & 0xFFU;
      sums[3] -= (pixel >> 24) & 0xFFU;
    }

    // Each box pass divides four channels per pixel by its window size. An integer division is
    // some twenty cycles, and six passes over a large region made those divisions the whole
    // cost of the blur, so the window size becomes a fixed-point reciprocal instead. Exact for
    // every window this code uses: sums stay under 255 * window.
    [[nodiscard]] std::uint64_t averageReciprocal(std::uint32_t count) { return (0xFFFFFFFFULL / count) + 1ULL; }

    [[nodiscard]] std::uint32_t averagePixel(const std::array<std::uint32_t, 4>& sums, std::uint64_t reciprocal) {
      const auto mean = [reciprocal](std::uint32_t sum) {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(sum) * reciprocal) >> 32U);
      };
      return (mean(sums[3]) << 24) | (mean(sums[2]) << 16) | (mean(sums[1]) << 8) | mean(sums[0]);
    }

    void boxBlurHorizontal(const std::uint32_t* src, std::uint32_t* dst, int width, int height, int radius) {
      const std::uint64_t reciprocal = averageReciprocal(static_cast<std::uint32_t>((radius * 2) + 1));
      const auto rowStride = static_cast<std::size_t>(width);
      for (int y = 0; y < height; ++y) {
        const std::uint32_t* srcRow = src + (static_cast<std::size_t>(y) * rowStride);
        std::uint32_t* dstRow = dst + (static_cast<std::size_t>(y) * rowStride);
        std::array<std::uint32_t, 4> sums{};
        for (int x = -radius; x <= radius; ++x) {
          addPixel(srcRow[static_cast<std::size_t>(std::clamp(x, 0, width - 1))], sums);
        }
        for (int x = 0; x < width; ++x) {
          dstRow[static_cast<std::size_t>(x)] = averagePixel(sums, reciprocal);
          if (x + 1 < width) {
            subtractPixel(srcRow[static_cast<std::size_t>(std::clamp(x - radius, 0, width - 1))], sums);
            addPixel(srcRow[static_cast<std::size_t>(std::clamp(x + radius + 1, 0, width - 1))], sums);
          }
        }
      }
    }

    void boxBlurVertical(const std::uint32_t* src, std::uint32_t* dst, int width, int height, int radius) {
      const std::uint64_t reciprocal = averageReciprocal(static_cast<std::uint32_t>((radius * 2) + 1));
      const auto rowStride = static_cast<std::size_t>(width);
      const auto sampleAt = [&](int row, int column) {
        return static_cast<std::size_t>(std::clamp(row, 0, height - 1)) * rowStride + static_cast<std::size_t>(column);
      };
      for (int x = 0; x < width; ++x) {
        std::array<std::uint32_t, 4> sums{};
        for (int y = -radius; y <= radius; ++y) {
          addPixel(src[sampleAt(y, x)], sums);
        }
        for (int y = 0; y < height; ++y) {
          dst[sampleAt(y, x)] = averagePixel(sums, reciprocal);
          if (y + 1 < height) {
            subtractPixel(src[sampleAt(y - radius, x)], sums);
            addPixel(src[sampleAt(y + radius + 1, x)], sums);
          }
        }
      }
    }

    // Three box passes approximating a Gaussian of the given sigma.
    void gaussianBoxRadii(double sigma, std::array<int, 3>& radii) {
      constexpr int kPassCount = 3;
      const double idealWidth = std::sqrt(((12.0 * sigma * sigma) / kPassCount) + 1.0);
      int lowerWidth = static_cast<int>(std::floor(idealWidth));
      if (lowerWidth % 2 == 0) {
        --lowerWidth;
      }
      lowerWidth = std::max(1, lowerWidth);
      const int upperWidth = lowerWidth + 2;
      const auto lowerPasses = std::clamp(
          static_cast<int>(std::round(
              ((12.0 * sigma * sigma)
               - (kPassCount * lowerWidth * lowerWidth)
               - (4.0 * kPassCount * lowerWidth)
               - (3.0 * kPassCount))
              / ((-4.0 * lowerWidth) - 4.0)
          )),
          0, kPassCount
      );
      for (int i = 0; i < kPassCount; ++i) {
        radii[static_cast<std::size_t>(i)] = ((i < lowerPasses ? lowerWidth : upperWidth) - 1) / 2;
      }
    }

    // A box blur costs the same per pixel whatever its radius, so a full-resolution pass over a
    // large region is pure waste: the result is band-limited to roughly sigma anyway. Averaging
    // step x step source blocks first and blurring that, then letting cairo scale the result
    // back, keeps the pass count off the region's real area and bounds it for a huge rect.
    [[nodiscard]] int blurDownscaleStep(double sigma, int regionWidth, int regionHeight) {
      const int fromSigma = std::clamp(static_cast<int>(sigma / 2.0), 1, kMaxBlurStep);
      // Padding is expressed in downscaled pixels, so a step near the region size would spend
      // more time gathering the halo than the region.
      const int fromRegion = std::max(1, std::min(regionWidth, regionHeight) / 8);
      int step = std::min(fromSigma, fromRegion);
      const auto area = static_cast<std::int64_t>(regionWidth) * regionHeight;
      while (step < kMaxBlurStep && area / (static_cast<std::int64_t>(step) * step) > kMaxBlurWorkPixels) {
        ++step;
      }
      return step;
    }

    [[nodiscard]] BlurResult
    blurRegion(cairo_surface_t* source, int regionX, int regionY, int regionWidth, int regionHeight, double strength) {
      cairo_surface_flush(source);
      const int srcWidth = cairo_image_surface_get_width(source);
      const int srcHeight = cairo_image_surface_get_height(source);
      const int srcStride = cairo_image_surface_get_stride(source);
      const unsigned char* srcData = cairo_image_surface_get_data(source);
      if (srcData == nullptr || srcWidth <= 0 || srcHeight <= 0) {
        return BlurResult{};
      }

      const double sigma = std::max(0.8, strength / 3.0);
      const int step = blurDownscaleStep(sigma, regionWidth, regionHeight);
      const int smallWidth = std::max(1, (regionWidth + step - 1) / step);
      const int smallHeight = std::max(1, (regionHeight + step - 1) / step);

      std::array<int, 3> radii{};
      gaussianBoxRadii(std::max(0.8, sigma / step), radii);
      const int padding = radii[0] + radii[1] + radii[2];
      const int workWidth = smallWidth + (padding * 2);
      const int workHeight = smallHeight + (padding * 2);

      // Reused across calls: a blur drag recomputes on every motion event and the churn of two
      // fresh multi-megabyte buffers per frame was most of its cost.
      static std::vector<std::uint32_t> pixels;
      static std::vector<std::uint32_t> scratch;
      const auto workRowStride = static_cast<std::size_t>(workWidth);
      const std::size_t workPixels = workRowStride * static_cast<std::size_t>(workHeight);
      pixels.resize(workPixels);
      scratch.resize(workPixels);

      // Four taps per block rather than a full step x step average: reading the whole region
      // costs more than every blur pass combined, and the passes that follow prefilter away
      // what the subsampling misses.
      const int nearTap = step / 4;
      const int farTap = (step * 3) / 4;
      const std::uint64_t tapReciprocal = averageReciprocal(4);
      const auto blockAverage = [&](int baseX, int baseY) {
        std::array<std::uint32_t, 4> sums{};
        for (const int dy : {nearTap, farTap}) {
          const int sampleY = std::clamp(baseY + dy, 0, srcHeight - 1);
          const auto* srcRow = reinterpret_cast<const std::uint32_t*>(
              srcData + (static_cast<std::size_t>(sampleY) * static_cast<std::size_t>(srcStride))
          );
          for (const int dx : {nearTap, farTap}) {
            addPixel(srcRow[static_cast<std::size_t>(std::clamp(baseX + dx, 0, srcWidth - 1))], sums);
          }
        }
        return averagePixel(sums, tapReciprocal);
      };

      for (int y = 0; y < workHeight; ++y) {
        std::uint32_t* workRow = pixels.data() + (static_cast<std::size_t>(y) * workRowStride);
        const int baseY = regionY + ((y - padding) * step);
        for (int x = 0; x < workWidth; ++x) {
          workRow[static_cast<std::size_t>(x)] = blockAverage(regionX + ((x - padding) * step), baseY);
        }
      }

      for (const int radius : radii) {
        boxBlurHorizontal(pixels.data(), scratch.data(), workWidth, workHeight, radius);
        boxBlurVertical(scratch.data(), pixels.data(), workWidth, workHeight, radius);
      }

      cairo_surface_t* result = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, smallWidth, smallHeight);
      if (cairo_surface_status(result) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(result);
        return BlurResult{};
      }
      unsigned char* dstData = cairo_image_surface_get_data(result);
      const int dstStride = cairo_image_surface_get_stride(result);
      for (int y = 0; y < smallHeight; ++y) {
        const std::uint32_t* srcRow =
            pixels.data() + (static_cast<std::size_t>(y + padding) * workRowStride) + static_cast<std::size_t>(padding);
        std::memcpy(
            dstData + (static_cast<std::size_t>(y) * static_cast<std::size_t>(dstStride)), srcRow,
            static_cast<std::size_t>(smallWidth) * sizeof(std::uint32_t)
        );
      }
      cairo_surface_mark_dirty(result);
      return BlurResult{.surface = result, .step = step};
    }

    [[nodiscard]] BlurResult cachedBlur(cairo_surface_t* source, int x, int y, int width, int height, double strength) {
      static std::uint64_t clock = 0;
      ++clock;

      auto& cache = blurCache();
      for (auto& entry : cache) {
        if (entry.surface != nullptr
            && entry.source == source
            && entry.x == x
            && entry.y == y
            && entry.width == width
            && entry.height == height
            && entry.strength == strength) {
          entry.stamp = clock;
          return BlurResult{.surface = entry.surface, .step = entry.step};
        }
      }

      const BlurResult blurred = blurRegion(source, x, y, width, height, strength);
      if (blurred.surface == nullptr) {
        return BlurResult{};
      }

      BlurCacheEntry* slot = cache.data();
      for (auto& entry : cache) {
        if (entry.surface == nullptr) {
          slot = &entry;
          break;
        }
        if (entry.stamp < slot->stamp) {
          slot = &entry;
        }
      }
      if (slot->surface != nullptr) {
        cairo_surface_destroy(slot->surface);
      }
      *slot = BlurCacheEntry{
          .surface = blurred.surface,
          .source = source,
          .x = x,
          .y = y,
          .width = width,
          .height = height,
          .step = blurred.step,
          .strength = strength,
          .stamp = clock,
      };
      return blurred;
    }

    void setSourceColor(cairo_t* cr, const AnnotationColor& color, double alphaOverride) {
      cairo_set_source_rgba(cr, color.r, color.g, color.b, alphaOverride);
    }

    void drawPolyline(cairo_t* cr, const std::vector<AnnotationPoint>& points) {
      cairo_move_to(cr, points.front().x, points.front().y);
      for (std::size_t i = 1; i < points.size(); ++i) {
        cairo_line_to(cr, points[i].x, points[i].y);
      }
    }

    void appendQuadraticCurve(cairo_t* cr, AnnotationPoint start, AnnotationPoint control, AnnotationPoint end) {
      constexpr double kControlScale = 2.0 / 3.0;
      cairo_curve_to(
          cr, start.x + ((control.x - start.x) * kControlScale), start.y + ((control.y - start.y) * kControlScale),
          end.x + ((control.x - end.x) * kControlScale), end.y + ((control.y - end.y) * kControlScale), end.x, end.y
      );
    }

    void drawSmoothPolyline(cairo_t* cr, const std::vector<AnnotationPoint>& points) {
      if (points.size() < 3) {
        drawPolyline(cr, points);
        return;
      }

      AnnotationPoint current = points.front();
      cairo_move_to(cr, current.x, current.y);
      for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        const AnnotationPoint end{
            .x = (points[i].x + points[i + 1].x) * 0.5,
            .y = (points[i].y + points[i + 1].y) * 0.5,
        };
        appendQuadraticCurve(cr, current, points[i], end);
        current = end;
      }
      appendQuadraticCurve(cr, current, points.back(), points.back());
    }

    void renderText(cairo_t* cr, const Annotation& annotation) {
      if (annotation.text.empty()) {
        return;
      }
      const TextMetrics metrics = textMetrics(annotation);
      PangoLayout* layout = pango_cairo_create_layout(cr);
      applyTextLayout(layout, annotation);
      cairo_move_to(cr, annotation.points.front().x + metrics.left, annotation.points.front().y + metrics.top);
      pango_cairo_show_layout(cr, layout);
      g_object_unref(layout);
    }

    void renderNumbering(cairo_t* cr, const Annotation& annotation) {
      const AnnotationPoint anchor = annotation.points.front();
      const double circleRadius = annotation.width / 2.0;
      cairo_arc(cr, anchor.x, anchor.y, circleRadius, 0.0, 2.0 * std::numbers::pi);
      cairo_fill(cr);

      Annotation label = annotation;
      label.width = annotation.width * 0.6;
      label.text = annotation.text.empty() ? std::string("?") : annotation.text;
      const TextMetrics metrics = textMetrics(label);

      const double luminance =
          (0.299 * annotation.stroke.r) + (0.587 * annotation.stroke.g) + (0.114 * annotation.stroke.b);
      const double shade = luminance > 0.5 ? 0.0 : 1.0;
      cairo_set_source_rgba(cr, shade, shade, shade, annotation.stroke.a);

      PangoLayout* layout = pango_cairo_create_layout(cr);
      applyTextLayout(layout, label);
      cairo_move_to(cr, anchor.x - (metrics.width / 2.0), anchor.y - (metrics.height / 2.0));
      pango_cairo_show_layout(cr, layout);
      g_object_unref(layout);
    }

    void renderBlur(cairo_t* cr, const Annotation& annotation, cairo_surface_t* background, double scale) {
      if (background == nullptr || cairo_surface_get_type(background) != CAIRO_SURFACE_TYPE_IMAGE) {
        return;
      }
      const AnnotationPoint start = annotation.points.front();
      const AnnotationPoint end = annotation.points.back();
      const double left = std::min(start.x, end.x);
      const double top = std::min(start.y, end.y);
      const double right = std::max(start.x, end.x);
      const double bottom = std::max(start.y, end.y);

      // The blur samples the background in its own pixel grid, so the logical rect is
      // mapped through the current transform rather than multiplied by `scale`.
      double deviceLeft = left;
      double deviceTop = top;
      double deviceRight = right;
      double deviceBottom = bottom;
      cairo_user_to_device(cr, &deviceLeft, &deviceTop);
      cairo_user_to_device(cr, &deviceRight, &deviceBottom);

      const int blurX = static_cast<int>(std::floor(std::min(deviceLeft, deviceRight)));
      const int blurY = static_cast<int>(std::floor(std::min(deviceTop, deviceBottom)));
      const int blurWidth = static_cast<int>(std::ceil(std::max(deviceLeft, deviceRight))) - blurX;
      const int blurHeight = static_cast<int>(std::ceil(std::max(deviceTop, deviceBottom))) - blurY;
      if (blurWidth <= 0 || blurHeight <= 0) {
        return;
      }

      const BlurResult blurred =
          cachedBlur(background, blurX, blurY, blurWidth, blurHeight, annotation.width * std::max(scale, 0.01));
      if (blurred.surface == nullptr) {
        return;
      }

      cairo_save(cr);
      cairo_rectangle(cr, left, top, right - left, bottom - top);
      cairo_clip(cr);
      cairo_identity_matrix(cr);
      cairo_translate(cr, blurX, blurY);
      cairo_scale(cr, blurred.step, blurred.step);
      cairo_set_source_surface(cr, blurred.surface, 0.0, 0.0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
      cairo_paint(cr);
      cairo_restore(cr);
    }

    void argb32ToStraightRgba(const unsigned char* src, int srcStride, std::uint8_t* dst, int width, int height) {
      for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            src + (static_cast<std::size_t>(y) * static_cast<std::size_t>(srcStride))
        );
        std::uint8_t* outRow = dst + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U);
        for (int x = 0; x < width; ++x) {
          const std::uint32_t pixel = row[static_cast<std::size_t>(x)];
          const auto a = static_cast<std::uint8_t>((pixel >> 24) & 0xFFU);
          auto r = static_cast<std::uint8_t>((pixel >> 16) & 0xFFU);
          auto g = static_cast<std::uint8_t>((pixel >> 8) & 0xFFU);
          auto b = static_cast<std::uint8_t>(pixel & 0xFFU);
          if (a != 0 && a != 255) {
            r = static_cast<std::uint8_t>(std::min(255, ((r * 255) + (a / 2)) / a));
            g = static_cast<std::uint8_t>(std::min(255, ((g * 255) + (a / 2)) / a));
            b = static_cast<std::uint8_t>(std::min(255, ((b * 255) + (a / 2)) / a));
          }
          outRow[(x * 4) + 0] = r;
          outRow[(x * 4) + 1] = g;
          outRow[(x * 4) + 2] = b;
          outRow[(x * 4) + 3] = a;
        }
      }
    }

  } // namespace

  AnnotationRect measureText(const Annotation& annotation) {
    if (annotation.points.empty()) {
      return AnnotationRect{};
    }
    const TextMetrics metrics = textMetrics(annotation);
    return AnnotationRect{
        .x = annotation.points.front().x + metrics.left,
        .y = annotation.points.front().y + metrics.top,
        .width = metrics.width,
        .height = metrics.height,
    };
  }

  void renderAnnotation(cairo_t* cr, const Annotation& annotation, cairo_surface_t* background, double scale) {
    if (annotation.points.empty()) {
      return;
    }

    cairo_new_path(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, annotation.width);
    setSourceColor(
        cr, annotation.stroke, annotation.tool == AnnotationTool::Highlighter ? kHighlighterAlpha : annotation.stroke.a
    );

    switch (annotation.tool) {
    case AnnotationTool::Move:
    case AnnotationTool::Eraser:
    case AnnotationTool::Crop:
      return;

    case AnnotationTool::Text:
      renderText(cr, annotation);
      return;

    case AnnotationTool::Numbering:
      renderNumbering(cr, annotation);
      return;

    case AnnotationTool::Highlighter: {
      const AnnotationPoint first = annotation.points.front();
      if (annotation.points.size() == 1) {
        cairo_rectangle(
            cr, first.x - (annotation.width / 4.0), first.y - (annotation.width / 2.0), annotation.width / 2.0,
            annotation.width
        );
        cairo_fill(cr);
        return;
      }
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
      drawPolyline(cr, annotation.points);
      cairo_stroke(cr);
      return;
    }

    case AnnotationTool::Blur:
      if (annotation.points.size() >= 2) {
        renderBlur(cr, annotation, background, scale);
      }
      return;

    case AnnotationTool::Rectangle:
    case AnnotationTool::Circle:
    case AnnotationTool::Line:
    case AnnotationTool::Arrow:
      break;

    case AnnotationTool::Brush:
      break;
    }

    if (annotationToolIsShape(annotation.tool) && annotation.points.size() >= 2) {
      const AnnotationPoint start = annotation.points.front();
      const AnnotationPoint end = annotation.points.back();
      const double left = std::min(start.x, end.x);
      const double top = std::min(start.y, end.y);
      const double rectWidth = std::abs(end.x - start.x);
      const double rectHeight = std::abs(end.y - start.y);

      switch (annotation.tool) {
      case AnnotationTool::Rectangle:
        cairo_rectangle(cr, left, top, rectWidth, rectHeight);
        break;
      case AnnotationTool::Circle: {
        const double radiusX = rectWidth / 2.0;
        const double radiusY = rectHeight / 2.0;
        cairo_save(cr);
        cairo_translate(cr, left + radiusX, top + radiusY);
        cairo_scale(cr, std::max(radiusX, 0.0001), std::max(radiusY, 0.0001));
        cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * std::numbers::pi);
        cairo_restore(cr);
        break;
      }
      case AnnotationTool::Line:
        cairo_move_to(cr, start.x, start.y);
        cairo_line_to(cr, end.x, end.y);
        cairo_stroke(cr);
        return;
      case AnnotationTool::Arrow: {
        const double angle = std::atan2(end.y - start.y, end.x - start.x);
        const double headSize = std::max(12.0, annotation.width * 3.0);
        constexpr double kHeadSpread = std::numbers::pi / 6.0;
        cairo_move_to(cr, start.x, start.y);
        cairo_line_to(cr, end.x, end.y);
        cairo_stroke(cr);
        cairo_move_to(cr, end.x, end.y);
        cairo_line_to(
            cr, end.x - (headSize * std::cos(angle - kHeadSpread)), end.y - (headSize * std::sin(angle - kHeadSpread))
        );
        cairo_move_to(cr, end.x, end.y);
        cairo_line_to(
            cr, end.x - (headSize * std::cos(angle + kHeadSpread)), end.y - (headSize * std::sin(angle + kHeadSpread))
        );
        cairo_stroke(cr);
        return;
      }
      default:
        return;
      }

      if (annotation.fill.a > 0.0F) {
        setSourceColor(cr, annotation.fill, annotation.fill.a);
        cairo_fill_preserve(cr);
        setSourceColor(cr, annotation.stroke, annotation.stroke.a);
      }
      cairo_stroke(cr);
      return;
    }

    if (annotation.points.size() == 1) {
      const AnnotationPoint point = annotation.points.front();
      cairo_arc(cr, point.x, point.y, annotation.width / 2.0, 0.0, 2.0 * std::numbers::pi);
      cairo_fill(cr);
      return;
    }

    drawSmoothPolyline(cr, annotation.points);
    cairo_stroke(cr);
  }

  void renderAnnotations(
      cairo_t* cr, std::span<const Annotation> annotations, cairo_surface_t* background, double scale,
      std::optional<std::size_t> skipIndex
  ) {
    for (std::size_t i = 0; i < annotations.size(); ++i) {
      if (skipIndex.has_value() && *skipIndex == i) {
        continue;
      }
      renderAnnotation(cr, annotations[i], background, scale);
    }
  }

  cairo_surface_t* frozenToCairo(const ScreencopyImage& image) {
    if (image.width <= 0 || image.height <= 0) {
      return nullptr;
    }
    const auto expected = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4U;
    if (image.rgba.size() < expected) {
      return nullptr;
    }

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, image.width, image.height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surface);
      return nullptr;
    }

    unsigned char* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < image.height; ++y) {
      auto* dstRow =
          reinterpret_cast<std::uint32_t*>(data + (static_cast<std::size_t>(y) * static_cast<std::size_t>(stride)));
      const std::uint8_t* srcRow =
          image.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) * 4U);
      for (int x = 0; x < image.width; ++x) {
        const std::uint32_t r = srcRow[(x * 4) + 0];
        const std::uint32_t g = srcRow[(x * 4) + 1];
        const std::uint32_t b = srcRow[(x * 4) + 2];
        const std::uint32_t a = srcRow[(x * 4) + 3];
        // cairo ARGB32 is premultiplied.
        const std::uint32_t pr = ((r * a) + 127) / 255;
        const std::uint32_t pg = ((g * a) + 127) / 255;
        const std::uint32_t pb = ((b * a) + 127) / 255;
        dstRow[static_cast<std::size_t>(x)] = (a << 24) | (pr << 16) | (pg << 8) | pb;
      }
    }
    cairo_surface_mark_dirty(surface);
    return surface;
  }

  void
  copyArgb32RectToRgba(cairo_surface_t* source, int x, int y, int width, int height, std::vector<std::uint8_t>& out) {
    out.clear();
    if (source == nullptr || width <= 0 || height <= 0) {
      return;
    }
    cairo_surface_flush(source);
    const unsigned char* data = cairo_image_surface_get_data(source);
    const int stride = cairo_image_surface_get_stride(source);
    const int surfaceWidth = cairo_image_surface_get_width(source);
    const int surfaceHeight = cairo_image_surface_get_height(source);
    if (data == nullptr || x < 0 || y < 0 || x + width > surfaceWidth || y + height > surfaceHeight) {
      return;
    }

    out.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    argb32ToStraightRgba(
        data + (static_cast<std::size_t>(y) * static_cast<std::size_t>(stride)) + (static_cast<std::size_t>(x) * 4U),
        stride, out.data(), width, height
    );
  }

  ScreencopyImage composeExport(
      const ScreencopyImage& background, double logicalWidth, double logicalHeight, const AnnotationDocument& document
  ) {
    // Blur samples the untouched capture, so the annotated canvas is a separate surface.
    cairo_surface_t* source = frozenToCairo(background);
    if (source == nullptr) {
      return ScreencopyImage{};
    }
    cairo_surface_t* canvas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, background.width, background.height);
    if (cairo_surface_status(canvas) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(canvas);
      cairo_surface_destroy(source);
      return ScreencopyImage{};
    }

    const double scaleX = logicalWidth > 0.0 ? static_cast<double>(background.width) / logicalWidth : 1.0;
    const double scaleY = logicalHeight > 0.0 ? static_cast<double>(background.height) / logicalHeight : 1.0;

    cairo_t* cr = cairo_create(canvas);
    cairo_set_source_surface(cr, source, 0.0, 0.0);
    cairo_paint(cr);
    cairo_scale(cr, scaleX, scaleY);
    renderAnnotations(cr, document.annotations(), source, std::min(scaleX, scaleY));
    cairo_destroy(cr);

    cairo_surface_t* result = canvas;
    if (const auto crop = document.crop(); crop.has_value() && crop->width > 0.0 && crop->height > 0.0) {
      const int cropX = std::clamp(static_cast<int>(std::round(crop->x * scaleX)), 0, background.width - 1);
      const int cropY = std::clamp(static_cast<int>(std::round(crop->y * scaleY)), 0, background.height - 1);
      const int cropWidth = std::clamp(static_cast<int>(std::round(crop->width * scaleX)), 1, background.width - cropX);
      const int cropHeight =
          std::clamp(static_cast<int>(std::round(crop->height * scaleY)), 1, background.height - cropY);

      cairo_surface_t* cropped = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cropWidth, cropHeight);
      if (cairo_surface_status(cropped) == CAIRO_STATUS_SUCCESS) {
        cairo_t* cropCr = cairo_create(cropped);
        cairo_set_source_surface(cropCr, canvas, -static_cast<double>(cropX), -static_cast<double>(cropY));
        cairo_paint(cropCr);
        cairo_destroy(cropCr);
        result = cropped;
      } else {
        cairo_surface_destroy(cropped);
      }
    }

    cairo_surface_flush(result);
    const int outWidth = cairo_image_surface_get_width(result);
    const int outHeight = cairo_image_surface_get_height(result);
    ScreencopyImage out{
        .width = outWidth,
        .height = outHeight,
        .yInvert = false,
        .rgba = {},
    };
    out.rgba.resize(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * 4U);
    argb32ToStraightRgba(
        cairo_image_surface_get_data(result), cairo_image_surface_get_stride(result), out.rgba.data(), outWidth,
        outHeight
    );

    if (result != canvas) {
      cairo_surface_destroy(result);
    }
    cairo_surface_destroy(canvas);
    cairo_surface_destroy(source);
    return out;
  }

  void clearBlurCache() {
    for (auto& entry : blurCache()) {
      if (entry.surface != nullptr) {
        cairo_surface_destroy(entry.surface);
      }
      entry = BlurCacheEntry{};
    }
  }

} // namespace capture
