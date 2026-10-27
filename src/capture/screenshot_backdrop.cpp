#include "capture/screenshot_backdrop.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace capture {

  namespace {

    struct Rgba {
      float r = 0.0F;
      float g = 0.0F;
      float b = 0.0F;
      float a = 0.0F;
    };

    // 4x4 ordered dither, so wide gradients do not band once quantized to 8 bits.
    constexpr std::array<std::array<float, 4>, 4> kBayer4 = {{
        {0.0F, 8.0F, 2.0F, 10.0F},
        {12.0F, 4.0F, 14.0F, 6.0F},
        {3.0F, 11.0F, 1.0F, 9.0F},
        {15.0F, 7.0F, 13.0F, 5.0F},
    }};

    // Anti-aliased coverage of a rounded rectangle at the pixel centre (px + 0.5, py + 0.5).
    [[nodiscard]] float roundedRectCoverage(int px, int py, float x0, float y0, float w, float h, float radius) {
      const float halfW = w * 0.5F;
      const float halfH = h * 0.5F;
      const float qx = std::abs(static_cast<float>(px) + 0.5F - (x0 + halfW)) - (halfW - radius);
      const float qy = std::abs(static_cast<float>(py) + 0.5F - (y0 + halfH)) - (halfH - radius);
      const float outside = std::hypot(std::max(qx, 0.0F), std::max(qy, 0.0F));
      const float inside = std::min(std::max(qx, qy), 0.0F);
      const float distance = outside + inside - radius;
      return std::clamp(0.5F - distance, 0.0F, 1.0F);
    }

    // One running-sum box blur pass over rows (stride 1) or columns (stride width).
    void boxBlurPass(
        const std::vector<float>& src, std::vector<float>& dst, int lines, int length, std::size_t lineStride,
        std::size_t step, int radius
    ) {
      const float scale = 1.0F / static_cast<float>((2 * radius) + 1);
      for (int line = 0; line < lines; ++line) {
        const std::size_t base = static_cast<std::size_t>(line) * lineStride;
        auto at = [&](int i) { return i < 0 || i >= length ? 0.0F : src[base + (static_cast<std::size_t>(i) * step)]; };
        float sum = 0.0F;
        for (int i = -radius; i <= radius; ++i) {
          sum += at(i);
        }
        for (int i = 0; i < length; ++i) {
          dst[base + (static_cast<std::size_t>(i) * step)] = sum * scale;
          sum += at(i + radius + 1) - at(i - radius);
        }
      }
    }

    // Three box passes per axis approximate a Gaussian with sigma = blur / 2.
    void gaussianBlur(std::vector<float>& mask, int width, int height, int blur) {
      const float sigma = static_cast<float>(blur) * 0.5F;
      if (sigma < 0.5F) {
        return;
      }
      constexpr int kPasses = 3;
      const float idealWidth = std::sqrt((12.0F * sigma * sigma / kPasses) + 1.0F);
      int lower = static_cast<int>(std::floor(idealWidth));
      if (lower % 2 == 0) {
        --lower;
      }
      const int upper = lower + 2;
      const auto passes = static_cast<float>(kPasses);
      const auto lowerF = static_cast<float>(lower);
      const float idealCount =
          ((12.0F * sigma * sigma) - (passes * lowerF * lowerF) - (4.0F * passes * lowerF) - (3.0F * passes))
          / ((-4.0F * lowerF) - 4.0F);
      const int lowerCount = static_cast<int>(std::round(idealCount));

      std::vector<float> scratch(mask.size());
      const auto w = static_cast<std::size_t>(width);
      for (int pass = 0; pass < kPasses; ++pass) {
        const int radius = ((pass < lowerCount ? lower : upper) - 1) / 2;
        if (radius <= 0) {
          continue;
        }
        boxBlurPass(mask, scratch, height, width, w, 1, radius);
        boxBlurPass(scratch, mask, width, height, 1, w, radius);
      }
    }

    // Straight-alpha source-over.
    [[nodiscard]] Rgba over(const Rgba& dst, float r, float g, float b, float a) {
      const float outA = a + (dst.a * (1.0F - a));
      if (outA <= 0.0F) {
        return {};
      }
      const float dstWeight = dst.a * (1.0F - a);
      return Rgba{
          .r = ((r * a) + (dst.r * dstWeight)) / outA,
          .g = ((g * a) + (dst.g * dstWeight)) / outA,
          .b = ((b * a) + (dst.b * dstWeight)) / outA,
          .a = outA,
      };
    }

    [[nodiscard]] std::uint8_t toByte(float value) {
      return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    }

  } // namespace

  std::optional<ScreenshotBackdrop> resolveScreenshotBackdrop(const ScreenshotBackdropSettings& config) {
    if (!config.enabled) {
      return std::nullopt;
    }
    return ScreenshotBackdrop{
        .gradient = config.gradient,
        .gradientStart = resolveColorSpec(config.gradientStart),
        .gradientEnd = resolveColorSpec(config.gradientEnd),
        .gradientAngleDegrees = config.gradientAngle,
        .padding = config.padding,
        .cornerRadius = config.cornerRadius,
        .shadow = config.shadow,
        .shadowColor = resolveColorSpec(config.shadowColor),
        .shadowOpacity = config.shadowOpacity,
        .shadowBlur = config.shadowBlur,
        .shadowOffsetX = config.shadowOffsetX,
        .shadowOffsetY = config.shadowOffsetY,
    };
  }

  ScreencopyImage applyScreenshotBackdrop(const ScreencopyImage& source, const ScreenshotBackdrop& backdrop) {
    if (source.width <= 0 || source.height <= 0 || source.rgba.empty()) {
      return source;
    }

    const int padding = std::max(backdrop.padding, 0);
    const int width = source.width + (2 * padding);
    const int height = source.height + (2 * padding);
    const auto pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    const auto x0 = static_cast<float>(padding);
    const auto y0 = static_cast<float>(padding);
    const auto imageW = static_cast<float>(source.width);
    const auto imageH = static_cast<float>(source.height);
    const float radius = std::clamp(static_cast<float>(backdrop.cornerRadius), 0.0F, std::min(imageW, imageH) * 0.5F);

    std::vector<float> shadowMask;
    const float shadowAlpha =
        std::clamp(backdrop.shadowOpacity, 0.0F, 1.0F) * std::clamp(backdrop.shadowColor.a, 0.0F, 1.0F);
    if (backdrop.shadow && shadowAlpha > 0.0F) {
      shadowMask.assign(pixelCount, 0.0F);
      const float sx = x0 + static_cast<float>(backdrop.shadowOffsetX);
      const float sy = y0 + static_cast<float>(backdrop.shadowOffsetY);
      const int minX = std::max(0, static_cast<int>(std::floor(sx)));
      const int minY = std::max(0, static_cast<int>(std::floor(sy)));
      const int maxX = std::min(width, static_cast<int>(std::ceil(sx + imageW)));
      const int maxY = std::min(height, static_cast<int>(std::ceil(sy + imageH)));
      for (int y = minY; y < maxY; ++y) {
        for (int x = minX; x < maxX; ++x) {
          shadowMask[(static_cast<std::size_t>(y) * width) + x] =
              roundedRectCoverage(x, y, sx, sy, imageW, imageH, radius);
        }
      }
      gaussianBlur(shadowMask, width, height, std::max(backdrop.shadowBlur, 0));
    }

    // Gradient parameter t is the pixel's projection onto the angle's direction, normalised so
    // the canvas corners span exactly 0..1.
    const float angle = backdrop.gradientAngleDegrees * std::numbers::pi_v<float> / 180.0F;
    // CSS convention: 0° points up, 90° left to right, 135° top-left to bottom-right.
    const float dirX = std::sin(angle);
    const float dirY = -std::cos(angle);
    const float extent = (std::abs(dirX) * static_cast<float>(width)) + (std::abs(dirY) * static_cast<float>(height));
    const float centreX = static_cast<float>(width) * 0.5F;
    const float centreY = static_cast<float>(height) * 0.5F;

    ScreencopyImage out{
        .width = width,
        .height = height,
        .yInvert = false,
        .rgba = std::vector<std::uint8_t>(pixelCount * 4U),
    };

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        Rgba pixel{};
        if (backdrop.gradient) {
          const float projected =
              ((static_cast<float>(x) + 0.5F - centreX) * dirX) + ((static_cast<float>(y) + 0.5F - centreY) * dirY);
          const float t = extent > 0.0F ? std::clamp((projected / extent) + 0.5F, 0.0F, 1.0F) : 0.0F;
          const float dither = ((kBayer4[y & 3][x & 3] + 0.5F) / 16.0F - 0.5F) / 255.0F;
          const Color& a = backdrop.gradientStart;
          const Color& b = backdrop.gradientEnd;
          pixel = over(
              Rgba{}, a.r + ((b.r - a.r) * t) + dither, a.g + ((b.g - a.g) * t) + dither,
              a.b + ((b.b - a.b) * t) + dither, std::clamp(a.a + ((b.a - a.a) * t), 0.0F, 1.0F)
          );
        }

        const std::size_t index = (static_cast<std::size_t>(y) * width) + x;
        if (!shadowMask.empty() && shadowMask[index] > 0.0F) {
          pixel = over(
              pixel, backdrop.shadowColor.r, backdrop.shadowColor.g, backdrop.shadowColor.b,
              shadowMask[index] * shadowAlpha
          );
        }

        const int sxPix = x - padding;
        const int syPix = y - padding;
        if (sxPix >= 0 && syPix >= 0 && sxPix < source.width && syPix < source.height) {
          const float coverage = roundedRectCoverage(x, y, x0, y0, imageW, imageH, radius);
          if (coverage > 0.0F) {
            const std::size_t src = ((static_cast<std::size_t>(syPix) * source.width) + sxPix) * 4U;
            const float alpha = (static_cast<float>(source.rgba[src + 3]) / 255.0F) * coverage;
            if (alpha >= 1.0F) {
              std::copy_n(
                  source.rgba.begin() + static_cast<std::ptrdiff_t>(src), 4,
                  out.rgba.begin() + static_cast<std::ptrdiff_t>(index * 4U)
              );
              continue;
            }
            pixel = over(
                pixel, static_cast<float>(source.rgba[src]) / 255.0F, static_cast<float>(source.rgba[src + 1]) / 255.0F,
                static_cast<float>(source.rgba[src + 2]) / 255.0F, alpha
            );
          }
        }

        out.rgba[(index * 4U) + 0] = toByte(pixel.r);
        out.rgba[(index * 4U) + 1] = toByte(pixel.g);
        out.rgba[(index * 4U) + 2] = toByte(pixel.b);
        out.rgba[(index * 4U) + 3] = toByte(pixel.a);
      }
    }
    return out;
  }

} // namespace capture
