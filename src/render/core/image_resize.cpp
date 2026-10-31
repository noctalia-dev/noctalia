#include "render/core/image_resize.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stb/stb_image_resize2.h>

namespace {

  [[nodiscard]] stbir_pixel_layout stbirSourceLayout(ResizeSourceLayout layout) noexcept {
    switch (layout) {
    case ResizeSourceLayout::Rgba:
      return STBIR_RGBA;
    case ResizeSourceLayout::RgbaPremultiplied:
    case ResizeSourceLayout::Rgbx:
      return STBIR_RGBA_PM;
    case ResizeSourceLayout::BgraPremultiplied:
    case ResizeSourceLayout::Bgrx:
      return STBIR_BGRA_PM;
    }
    return STBIR_RGBA;
  }

  [[nodiscard]] std::uint8_t straightChannel(std::uint32_t value, std::uint32_t alpha) noexcept {
    return alpha == 0 ? 0 : static_cast<std::uint8_t>(std::min(255U, (value * 255U + alpha / 2U) / alpha));
  }

  [[nodiscard]] bool fitsInMemory(int width, int height) noexcept {
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    return w <= std::numeric_limits<std::size_t>::max() / 4U / h;
  }

} // namespace

ImageExtent fitImageWithin(int width, int height, int maxWidth, int maxHeight) noexcept {
  if (width <= 0 || height <= 0 || maxWidth <= 0 || maxHeight <= 0) {
    return {.width = std::max(width, 0), .height = std::max(height, 0)};
  }
  const float scale = std::min(
      {1.0F, static_cast<float>(maxWidth) / static_cast<float>(width),
       static_cast<float>(maxHeight) / static_cast<float>(height)}
  );
  if (scale >= 1.0F) {
    return {.width = width, .height = height};
  }
  return {
      .width = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale))),
      .height = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale))),
  };
}

std::vector<std::uint8_t> resizeToRgba(
    const std::uint8_t* source, int sourceWidth, int sourceHeight, ResizeSourceLayout layout, int targetWidth,
    int targetHeight
) {
  if (source == nullptr
      || sourceWidth <= 0
      || sourceHeight <= 0
      || targetWidth <= 0
      || targetHeight <= 0
      || !fitsInMemory(sourceWidth, sourceHeight)
      || !fitsInMemory(targetWidth, targetHeight)) {
    return {};
  }

  std::vector<std::uint8_t> out(static_cast<std::size_t>(targetWidth) * static_cast<std::size_t>(targetHeight) * 4U);

  // Premultiplied and opaque sources stay premultiplied through stbir, which then only filters and swizzles. stbir
  // also skips alpha conversion when it point-samples an unchanged size, so straightening is done here instead.
  const bool straightSource = layout == ResizeSourceLayout::Rgba;
  STBIR_RESIZE resize;
  stbir_resize_init(
      &resize, source, sourceWidth, sourceHeight, 0, out.data(), targetWidth, targetHeight, 0, STBIR_RGBA,
      STBIR_TYPE_UINT8_SRGB
  );
  stbir_set_pixel_layouts(&resize, stbirSourceLayout(layout), straightSource ? STBIR_RGBA : STBIR_RGBA_PM);
  if (stbir_resize_extended(&resize) == 0) {
    return {};
  }
  if (straightSource) {
    return out;
  }

  const bool opaque = layout == ResizeSourceLayout::Rgbx || layout == ResizeSourceLayout::Bgrx;
  for (std::size_t i = 0; i < out.size(); i += 4U) {
    std::uint8_t* pixel = out.data() + i;
    if (opaque) {
      pixel[3] = 255;
      continue;
    }
    const std::uint32_t alpha = pixel[3];
    if (alpha == 255) {
      continue;
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
      pixel[channel] = straightChannel(pixel[channel], alpha);
    }
  }
  return out;
}
