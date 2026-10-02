#include "shell/dock/icon_shadow.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace shell::dock {

  LoadedImageFile makeIconShadow(const LoadedImageFile& source, int iconSize, int density) {
    if (iconSize <= 0
        || density <= 0
        || source.width <= 0
        || source.height <= 0
        || source.rgba.size() != static_cast<std::size_t>(source.width) * source.height * 4U) {
      return {};
    }
    const int size = iconSize * density;
    const int pad = kIconShadowPadding * density;
    const int width = size + pad * 2;
    const float sigma = 2.0F * static_cast<float>(density);
    const auto sourceWidth = static_cast<float>(source.width);
    const auto sourceHeight = static_cast<float>(source.height);
    const float scale = static_cast<float>(size) / std::max(sourceWidth, sourceHeight);
    const float left = (static_cast<float>(size) - sourceWidth * scale) * 0.5F;
    const float top = (static_cast<float>(size) - sourceHeight * scale) * 0.5F;
    const auto count = static_cast<std::size_t>(width) * width;
    std::vector<float> alpha(count), horizontal(count);
    const auto sample = [&](int x, int y) {
      x = std::clamp(x, 0, source.width - 1);
      y = std::clamp(y, 0, source.height - 1);
      return source.rgba[(static_cast<std::size_t>(y) * source.width + x) * 4U + 3U] / 255.0F;
    };
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        const float u = (static_cast<float>(x) + 0.5F - left) / scale;
        const float v = (static_cast<float>(y) + 0.5F - top) / scale;
        if (u < 0 || u > sourceWidth || v < 0 || v > sourceHeight) {
          continue;
        }
        const int ix = static_cast<int>(std::floor(u - 0.5F));
        const int iy = static_cast<int>(std::floor(v - 0.5F));
        const float fx = u - 0.5F - static_cast<float>(ix);
        const float fy = v - 0.5F - static_cast<float>(iy);
        alpha[static_cast<std::size_t>(y + pad) * width + x + pad] = std::lerp(
            std::lerp(sample(ix, iy), sample(ix + 1, iy), fx),
            std::lerp(sample(ix, iy + 1), sample(ix + 1, iy + 1), fx), fy
        );
      }
    }
    std::vector<float> kernel(pad * 2 + 1);
    float sum = 0;
    for (int i = -pad; i <= pad; ++i) {
      kernel[i + pad] = std::exp(-static_cast<float>(i * i) / (2.0F * sigma * sigma));
      sum += kernel[i + pad];
    }
    for (auto& weight : kernel) {
      weight /= sum;
    }
    for (int y = 0; y < width; ++y) {
      for (int x = 0; x < width; ++x) {
        for (int dx = std::max(-pad, -x); dx <= std::min(pad, width - 1 - x); ++dx) {
          horizontal[static_cast<std::size_t>(y) * width + x] +=
              alpha[static_cast<std::size_t>(y) * width + x + dx] * kernel[dx + pad];
        }
      }
    }
    LoadedImageFile result{.rgba = std::vector<std::uint8_t>(count * 4U, 255), .width = width, .height = width};
    for (int y = 0; y < width; ++y) {
      for (int x = 0; x < width; ++x) {
        float value = 0;
        for (int dy = std::max(-pad, -y); dy <= std::min(pad, width - 1 - y); ++dy) {
          value += horizontal[static_cast<std::size_t>(y + dy) * width + x] * kernel[dy + pad];
        }
        result.rgba[(static_cast<std::size_t>(y) * width + x) * 4U + 3U] =
            static_cast<std::uint8_t>(std::round(std::clamp(value, 0.0F, 1.0F) * 255));
      }
    }
    return result;
  }

} // namespace shell::dock
