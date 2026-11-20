#include "render/core/image_effects.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

  std::size_t pixelIndex(int x, int y, int width, int channel) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4U
        + static_cast<std::size_t>(channel);
  }

  void boxBlurHorizontal(
      const std::vector<std::uint8_t>& source, std::vector<std::uint8_t>& destination, int width, int height, int radius
  ) {
    const auto diameter = static_cast<std::uint32_t>(radius * 2 + 1);
    for (int y = 0; y < height; ++y) {
      for (int channel = 0; channel < 4; ++channel) {
        std::uint32_t sum = 0;
        for (int offset = -radius; offset <= radius; ++offset) {
          sum += source[pixelIndex(std::clamp(offset, 0, width - 1), y, width, channel)];
        }
        for (int x = 0; x < width; ++x) {
          destination[pixelIndex(x, y, width, channel)] = static_cast<std::uint8_t>((sum + diameter / 2U) / diameter);
          const int removeX = std::clamp(x - radius, 0, width - 1);
          const int addX = std::clamp(x + radius + 1, 0, width - 1);
          sum = sum - source[pixelIndex(removeX, y, width, channel)] + source[pixelIndex(addX, y, width, channel)];
        }
      }
    }
  }

  void boxBlurVertical(
      const std::vector<std::uint8_t>& source, std::vector<std::uint8_t>& destination, int width, int height, int radius
  ) {
    const auto diameter = static_cast<std::uint32_t>(radius * 2 + 1);
    for (int x = 0; x < width; ++x) {
      for (int channel = 0; channel < 4; ++channel) {
        std::uint32_t sum = 0;
        for (int offset = -radius; offset <= radius; ++offset) {
          sum += source[pixelIndex(x, std::clamp(offset, 0, height - 1), width, channel)];
        }
        for (int y = 0; y < height; ++y) {
          destination[pixelIndex(x, y, width, channel)] = static_cast<std::uint8_t>((sum + diameter / 2U) / diameter);
          const int removeY = std::clamp(y - radius, 0, height - 1);
          const int addY = std::clamp(y + radius + 1, 0, height - 1);
          sum = sum - source[pixelIndex(x, removeY, width, channel)] + source[pixelIndex(x, addY, width, channel)];
        }
      }
    }
  }

  float pixelLuminance(float red, float green, float blue) noexcept {
    return 0.299F * red + 0.587F * green + 0.114F * blue;
  }

  std::uint8_t toByte(float level) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(level, 0.0F, 1.0F) * 255.0F));
  }

} // namespace

namespace image_effects {

  void blur(std::vector<std::uint8_t>& rgba, int width, int height, int radius) {
    if (rgba.empty() || width <= 0 || height <= 0 || radius <= 0) {
      return;
    }
    const int passRadius = std::max(1, radius / 2);
    std::vector<std::uint8_t> scratch(rgba.size());
    constexpr int kBlurPasses = 3;
    for (int pass = 0; pass < kBlurPasses; ++pass) {
      boxBlurHorizontal(rgba, scratch, width, height, passRadius);
      boxBlurVertical(scratch, rgba, width, height, passRadius);
    }
  }

  void colorizeAppIcon(std::uint8_t* rgba, int width, int height, const Color& tint, bool lightTheme) noexcept {
    if (rgba == nullptr || width <= 0 || height <= 0) {
      return;
    }

    constexpr float kAlphaCutoff = 8.0F / 255.0F;
    constexpr float kMinContrastSpan = 0.12F;
    const float minDisplayLevel = lightTheme ? 0.32F : 0.28F;
    const float tintLum = relativeLuminance(tint);

    float minLum = 1.0F;
    float maxLum = 0.0F;
    float inkSum = 0.0F;
    float inkWeight = 0.0F;
    bool found = false;

    const auto pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < pixelCount; ++i) {
      const std::size_t offset = i * 4U;
      const float alpha = static_cast<float>(rgba[offset + 3U]) / 255.0F;
      if (alpha < kAlphaCutoff) {
        continue;
      }

      const float lum = pixelLuminance(
          static_cast<float>(rgba[offset]) / 255.0F, static_cast<float>(rgba[offset + 1U]) / 255.0F,
          static_cast<float>(rgba[offset + 2U]) / 255.0F
      );
      minLum = std::min(minLum, lum);
      maxLum = std::max(maxLum, lum);
      inkSum += lum * alpha;
      inkWeight += alpha;
      found = true;
    }

    if (!found) {
      return;
    }

    const float meanInk = inkWeight > 0.0F ? inkSum / inkWeight : 0.5F;
    const bool invertInk = (tintLum < 0.45F && meanInk > 0.55F) || (tintLum > 0.55F && meanInk < 0.45F);

    const float rawSpan = maxLum - minLum;
    const bool stretch = rawSpan >= kMinContrastSpan;
    const float span = std::max(rawSpan, 1e-3F);
    const float pad = span * 0.04F;
    const float minMapped = std::max(0.0F, minLum - pad);
    const float mappedSpan = std::max(maxLum + pad - minMapped, 1e-3F);

    for (std::size_t i = 0; i < pixelCount; ++i) {
      const std::size_t offset = i * 4U;
      const float alpha = static_cast<float>(rgba[offset + 3U]) / 255.0F;
      if (alpha < kAlphaCutoff) {
        continue;
      }

      const float lum = pixelLuminance(
          static_cast<float>(rgba[offset]) / 255.0F, static_cast<float>(rgba[offset + 1U]) / 255.0F,
          static_cast<float>(rgba[offset + 2U]) / 255.0F
      );
      float level = stretch ? std::clamp((lum - minMapped) / mappedSpan, 0.0F, 1.0F) : lum;
      if (invertInk) {
        level = 1.0F - level;
      }
      const float displayLevel = minDisplayLevel + (1.0F - minDisplayLevel) * level;
      rgba[offset] = toByte(tint.r * displayLevel);
      rgba[offset + 1U] = toByte(tint.g * displayLevel);
      rgba[offset + 2U] = toByte(tint.b * displayLevel);
    }
  }

  void symbolicMask(std::vector<std::uint8_t>& rgba, const Color& color) noexcept {
    const std::uint8_t rr = toByte(color.r);
    const std::uint8_t gg = toByte(color.g);
    const std::uint8_t bb = toByte(color.b);

    std::size_t transparentPixels = 0;
    float lumSum = 0.0F;
    float alphaWeight = 0.0F;
    const std::size_t pixelCount = rgba.size() / 4U;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
      const std::uint8_t a = rgba[i + 3];
      if (a <= 8) {
        ++transparentPixels;
        continue;
      }
      const float lum = pixelLuminance(
          static_cast<float>(rgba[i]) / 255.0F, static_cast<float>(rgba[i + 1]) / 255.0F,
          static_cast<float>(rgba[i + 2]) / 255.0F
      );
      const float w = static_cast<float>(a) / 255.0F;
      lumSum += lum * w;
      alphaWeight += w;
    }

    // Mostly transparent sources are proper alpha masks; opaque monochrome rasters encode the shape in luminance,
    // with polarity taken from the average.
    const float transparentRatio =
        pixelCount == 0 ? 0.0F : static_cast<float>(transparentPixels) / static_cast<float>(pixelCount);
    const bool useSourceAlphaMask = transparentRatio > 0.10F;
    const float avgLum = alphaWeight > 0.0F ? lumSum / alphaWeight : 0.0F;
    const bool invertLumaMask = avgLum > 0.5F;

    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
      const std::uint8_t a = rgba[i + 3];
      if (a == 0) {
        continue;
      }
      const float lum = pixelLuminance(
          static_cast<float>(rgba[i]) / 255.0F, static_cast<float>(rgba[i + 1]) / 255.0F,
          static_cast<float>(rgba[i + 2]) / 255.0F
      );
      const float mask = useSourceAlphaMask ? 1.0F : (invertLumaMask ? (1.0F - lum) : lum);
      rgba[i + 0] = rr;
      rgba[i + 1] = gg;
      rgba[i + 2] = bb;
      rgba[i + 3] = static_cast<std::uint8_t>(std::lround(static_cast<float>(a) * std::clamp(mask, 0.0F, 1.0F)));
    }
  }

} // namespace image_effects
