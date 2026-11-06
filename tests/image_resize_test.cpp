#include "render/core/image_resize.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  void requireNear(std::span<const std::uint8_t> actual, std::span<const std::uint8_t> expected) {
    require(actual.size() == expected.size(), "resized output has the wrong byte count");
    for (std::size_t i = 0; i < actual.size(); ++i) {
      require(std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i])) <= 1, "resized channels differ");
    }
  }

  void rgbMatchesOpaqueRgba() {
    // Tightly packed odd-width rows have no fourth channel or row padding to read.
    constexpr int width = 5;
    constexpr int height = 3;
    std::array<std::uint8_t, width * height * 3> rgb{};
    std::array<std::uint8_t, width * height * 4> rgba{};
    for (std::size_t i = 0; i < rgb.size() / 3; ++i) {
      rgb[i * 3] = static_cast<std::uint8_t>((i * 31) % 256);
      rgb[i * 3 + 1] = static_cast<std::uint8_t>((i * 67 + 13) % 256);
      rgb[i * 3 + 2] = static_cast<std::uint8_t>((i * 101 + 29) % 256);
      for (std::size_t channel = 0; channel < 3; ++channel) {
        rgba[i * 4 + channel] = rgb[i * 3 + channel];
      }
      rgba[i * 4 + 3] = 255;
    }
    constexpr std::array targets{
        ImageExtent{5, 3}, ImageExtent{2, 1}, ImageExtent{1, 3}, ImageExtent{5, 1}, ImageExtent{9, 7}
    };
    for (const auto target : targets) {
      const auto actual = resizeToRgba(rgb.data(), width, height, ResizeSourceLayout::Rgb, target.width, target.height);
      const auto reference =
          resizeToRgba(rgba.data(), width, height, ResizeSourceLayout::Rgba, target.width, target.height);
      requireNear(actual, reference);
      for (std::size_t i = 3; i < actual.size(); i += 4) {
        require(actual[i] == 255, "RGB resize did not produce opaque alpha");
      }
    }

    const std::array<std::uint8_t, 3> pixel{23, 127, 231};
    const auto enlarged = resizeToRgba(pixel.data(), 1, 1, ResizeSourceLayout::Rgb, 3, 2);
    const std::array<std::uint8_t, 4> expected{23, 127, 231, 255};
    require(enlarged.size() == 24, "single RGB pixel enlarged to the wrong size");
    for (std::size_t i = 0; i < enlarged.size(); i += 4) {
      requireNear(std::span(enlarged).subspan(i, 4), expected);
    }

    const std::array<std::uint8_t, 6> blackWhite{0, 0, 0, 255, 255, 255};
    const auto average = resizeToRgba(blackWhite.data(), 2, 1, ResizeSourceLayout::Rgb, 1, 1);
    require(average.size() == 4 && average[0] >= 185 && average[0] <= 190, "RGB filtering lost sRGB awareness");
    require(
        average[0] == average[1] && average[1] == average[2] && average[3] == 255,
        "RGB filtering changed channel order or opacity"
    );
  }

  void preserveAlphaAndChannelLayouts() {
    const std::array<std::uint8_t, 8> transparentRedBlue{255, 0, 0, 0, 0, 0, 255, 255};
    const auto resized = resizeToRgba(transparentRedBlue.data(), 2, 1, ResizeSourceLayout::Rgba, 1, 1);
    require(
        resized.size() == 4
            && resized[0] < 5
            && resized[1] == 0
            && resized[2] > 250
            && resized[3] >= 125
            && resized[3] <= 130,
        "straight-alpha filtering introduced a transparent-edge halo"
    );
    requireNear(resizeToRgba(transparentRedBlue.data(), 2, 1, ResizeSourceLayout::Rgba, 2, 1), transparentRedBlue);

    const std::array<std::uint8_t, 8> rgbaPm{0, 0, 0, 0, 0, 0, 255, 255};
    const std::array<std::uint8_t, 8> bgraPm{0, 0, 0, 0, 255, 0, 0, 255};
    requireNear(resizeToRgba(rgbaPm.data(), 2, 1, ResizeSourceLayout::RgbaPremultiplied, 1, 1), resized);
    requireNear(resizeToRgba(bgraPm.data(), 2, 1, ResizeSourceLayout::BgraPremultiplied, 1, 1), resized);
    const std::array<std::uint8_t, 4> translucent{32, 64, 96, 128};
    const std::array<std::uint8_t, 4> translucentBgra{96, 64, 32, 128};
    const std::array<std::uint8_t, 4> straight{64, 128, 191, 128};
    requireNear(resizeToRgba(translucent.data(), 1, 1, ResizeSourceLayout::RgbaPremultiplied, 1, 1), straight);
    requireNear(resizeToRgba(translucentBgra.data(), 1, 1, ResizeSourceLayout::BgraPremultiplied, 1, 1), straight);

    const std::array<std::uint8_t, 6> rgb{17, 91, 231, 209, 43, 8};
    const std::array<std::uint8_t, 8> rgbx{17, 91, 231, 0, 209, 43, 8, 73};
    const std::array<std::uint8_t, 8> bgrx{231, 91, 17, 0, 8, 43, 209, 73};
    for (const int target : {1, 2, 5}) {
      const auto opaque = resizeToRgba(rgb.data(), 2, 1, ResizeSourceLayout::Rgb, target, 1);
      requireNear(resizeToRgba(rgbx.data(), 2, 1, ResizeSourceLayout::Rgbx, target, 1), opaque);
      requireNear(resizeToRgba(bgrx.data(), 2, 1, ResizeSourceLayout::Bgrx, target, 1), opaque);
    }
  }

  void rejectInvalidExtents() {
    const std::array<std::uint8_t, 3> pixel{1, 2, 3};
    require(resizeToRgba(nullptr, 1, 1, ResizeSourceLayout::Rgb, 1, 1).empty(), "null source was accepted");
    constexpr std::array invalid{
        std::array{0, 1, 1, 1}, std::array{1, -1, 1, 1}, std::array{1, 1, 0, 1}, std::array{1, 1, 1, -1}
    };
    for (const auto extent : invalid) {
      require(
          resizeToRgba(pixel.data(), extent[0], extent[1], ResizeSourceLayout::Rgb, extent[2], extent[3]).empty(),
          "invalid extent was accepted"
      );
    }
  }
} // namespace

int main() {
  try {
    rgbMatchesOpaqueRgba();
    preserveAlphaAndChannelLayouts();
    rejectInvalidExtents();
  } catch (const std::exception& error) {
    std::println(stderr, "image_resize_test: {}", error.what());
    return 1;
  }
  return 0;
}
