#include "theme/image_loader.h"

#include "render/core/image_decoder.h"
#include "theme/scheme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <iterator>

namespace noctalia::theme {

  namespace {

    constexpr int kTarget = 112;

    std::expected<std::vector<uint8_t>, std::string> readFile(std::string_view path) {
      std::ifstream f(std::string(path), std::ios::binary);
      if (!f) {
        return std::unexpected("cannot open file");
      }
      return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    // Hand-port of `image::imageops::sample::resize` (Triangle filter) from
    // the Rust `image` crate, which is what matugen / material_colors uses.
    // We need byte-for-byte parity with that implementation because a few
    // LSB of drift in the resized 112×112 buffer can move the seed to a
    // different cluster.
    //
    // Algorithm: separable scale-aware tent filter, vertical pass first
    // into a float intermediate (no clamping/rounding between passes), then
    // horizontal pass with clamp+round-to-nearest into u8. For downsampling
    // the kernel support widens by the downsample ratio. Operates in 8-bit
    // sRGB space (no linearisation), matching image crate.
    //
    // Pixel-center convention: input pixels are at integer positions; the
    // output pixel `o` corresponds to input center `(o + 0.5) * ratio`. The
    // kernel is evaluated at `(i - (inputCenter - 0.5)) / sratio` for input
    // pixel index `i`. See sample.rs in the image crate for the canonical
    // form.
    inline float triangleKernel(float x) {
      x = x < 0 ? -x : x;
      return x < 1.0f ? 1.0f - x : 0.0f;
    }

    void verticalBoxSampleU8ToF32(const uint8_t* src, int srcW, int srcH, float* dst, int dstH) {
      constexpr double kEpsilon = 1.0e-12;
      const double factor = static_cast<double>(dstH) / static_cast<double>(srcH);
      double scale = std::max(1.0 / factor + kEpsilon, 1.0);
      double support = scale * 0.5;
      if (support < 0.5) {
        support = 0.5;
        scale = 1.0;
      }
      scale = 1.0 / scale;

      for (int outy = 0; outy < dstH; ++outy) {
        const double bisect = (static_cast<double>(outy) + 0.5) / factor + kEpsilon;
        const int start = static_cast<int>(std::max(bisect - support + 0.5, 0.0));
        const int stop = static_cast<int>(std::min(bisect + support + 0.5, static_cast<double>(srcH)));
        const int count = stop - start;
        if (count <= 0)
          continue;
        const float weight = 1.0f / static_cast<float>(count);

        for (int x = 0; x < srcW; ++x) {
          float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;
          for (int i = start; i < stop; ++i) {
            const float sampleWeight = weight;
            const uint8_t* p = src + (i * srcW + x) * 4;
            t0 += static_cast<float>(p[0]) * sampleWeight;
            t1 += static_cast<float>(p[1]) * sampleWeight;
            t2 += static_cast<float>(p[2]) * sampleWeight;
            t3 += static_cast<float>(p[3]) * sampleWeight;
          }
          float* dp = dst + (outy * srcW + x) * 4;
          dp[0] = t0;
          dp[1] = t1;
          dp[2] = t2;
          dp[3] = t3;
        }
      }
      (void)scale;
    }

    void horizontalBoxSampleF32ToU8(const float* src, int srcW, int srcH, uint8_t* dst, int dstW) {
      constexpr double kEpsilon = 1.0e-12;
      const double factor = static_cast<double>(dstW) / static_cast<double>(srcW);
      double scale = std::max(1.0 / factor + kEpsilon, 1.0);
      double support = scale * 0.5;
      if (support < 0.5) {
        support = 0.5;
        scale = 1.0;
      }
      scale = 1.0 / scale;

      auto toU8 = [](float v) -> uint8_t {
        v = std::clamp(v, 0.0f, 255.0f);
        return static_cast<uint8_t>(std::floor(v + 0.5f));
      };

      for (int outx = 0; outx < dstW; ++outx) {
        const double bisect = (static_cast<double>(outx) + 0.5) / factor + kEpsilon;
        const int start = static_cast<int>(std::max(bisect - support + 0.5, 0.0));
        const int stop = static_cast<int>(std::min(bisect + support + 0.5, static_cast<double>(srcW)));
        const int count = stop - start;
        if (count <= 0)
          continue;
        const float weight = 1.0f / static_cast<float>(count);

        for (int y = 0; y < srcH; ++y) {
          float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;
          for (int i = start; i < stop; ++i) {
            const float* p = src + (y * srcW + i) * 4;
            t0 += p[0] * weight;
            t1 += p[1] * weight;
            t2 += p[2] * weight;
            t3 += p[3] * weight;
          }

          uint8_t* dp = dst + (y * dstW + outx) * 4;
          dp[0] = toU8(t0);
          dp[1] = toU8(t1);
          dp[2] = toU8(t2);
          dp[3] = toU8(t3);
        }
      }
      (void)scale;
    }

    // Vertical pass: src is RGBA u8 (srcW × srcH), dst is RGBA f32 (srcW × dstH).
    void verticalSampleU8ToF32(const uint8_t* src, int srcW, int srcH, float* dst, int dstH) {
      const float ratio = static_cast<float>(srcH) / static_cast<float>(dstH);
      const float sratio = ratio < 1.0f ? 1.0f : ratio;
      const float srcSupport = 1.0f * sratio;

      std::vector<float> ws;
      for (int outy = 0; outy < dstH; ++outy) {
        const float inputyOrig = (static_cast<float>(outy) + 0.5f) * ratio;
        int left = static_cast<int>(std::floor(inputyOrig - srcSupport));
        int right = static_cast<int>(std::ceil(inputyOrig + srcSupport));
        left = std::max(left, 0);
        left = std::min(left, srcH - 1);
        right = std::max(right, left + 1);
        right = std::min(right, srcH);
        const float inputy = inputyOrig - 0.5f;

        ws.clear();
        float sum = 0.0f;
        for (int i = left; i < right; ++i) {
          float w = triangleKernel((static_cast<float>(i) - inputy) / sratio);
          ws.push_back(w);
          sum += w;
        }
        for (auto& w : ws)
          w /= sum;

        for (int x = 0; x < srcW; ++x) {
          float t0 = 0, t1 = 0, t2 = 0, t3 = 0;
          for (int k = 0; k < static_cast<int>(ws.size()); ++k) {
            const auto weightIndex = static_cast<std::size_t>(k);
            const uint8_t* p = src + ((left + k) * srcW + x) * 4;
            const float w = ws[weightIndex];
            t0 += static_cast<float>(p[0]) * w;
            t1 += static_cast<float>(p[1]) * w;
            t2 += static_cast<float>(p[2]) * w;
            t3 += static_cast<float>(p[3]) * w;
          }
          float* dp = dst + (outy * srcW + x) * 4;
          // No clamp / no round — image crate's vertical_sample writes raw
          // f32 into Rgba32FImage.
          dp[0] = t0;
          dp[1] = t1;
          dp[2] = t2;
          dp[3] = t3;
        }
      }
    }

    // Horizontal pass: src is RGBA f32 (srcW × srcH), dst is RGBA u8 (dstW × srcH).
    void horizontalSampleF32ToU8(const float* src, int srcW, int srcH, uint8_t* dst, int dstW) {
      const float ratio = static_cast<float>(srcW) / static_cast<float>(dstW);
      const float sratio = ratio < 1.0f ? 1.0f : ratio;
      const float srcSupport = 1.0f * sratio;

      std::vector<float> ws;
      for (int outx = 0; outx < dstW; ++outx) {
        const float inputxOrig = (static_cast<float>(outx) + 0.5f) * ratio;
        int left = static_cast<int>(std::floor(inputxOrig - srcSupport));
        int right = static_cast<int>(std::ceil(inputxOrig + srcSupport));
        left = std::max(left, 0);
        left = std::min(left, srcW - 1);
        right = std::max(right, left + 1);
        right = std::min(right, srcW);
        const float inputx = inputxOrig - 0.5f;

        ws.clear();
        float sum = 0.0f;
        for (int i = left; i < right; ++i) {
          float w = triangleKernel((static_cast<float>(i) - inputx) / sratio);
          ws.push_back(w);
          sum += w;
        }
        for (auto& w : ws)
          w /= sum;

        for (int y = 0; y < srcH; ++y) {
          float t0 = 0, t1 = 0, t2 = 0, t3 = 0;
          for (int k = 0; k < static_cast<int>(ws.size()); ++k) {
            const auto weightIndex = static_cast<std::size_t>(k);
            const float* p = src + (y * srcW + (left + k)) * 4;
            const float w = ws[weightIndex];
            t0 += p[0] * w;
            t1 += p[1] * w;
            t2 += p[2] * w;
            t3 += p[3] * w;
          }
          // FloatNearest(clamp(t, 0, 255)) → u8. Rust's f32::round is
          // round-half-away-from-zero.
          auto toU8 = [](float v) -> uint8_t {
            v = std::clamp(v, 0.0f, 255.0f);
            return static_cast<uint8_t>(std::floor(v + 0.5f));
          };
          uint8_t* dp = dst + (y * dstW + outx) * 4;
          dp[0] = toU8(t0);
          dp[1] = toU8(t1);
          dp[2] = toU8(t2);
          dp[3] = toU8(t3);
        }
      }
    }

    std::vector<uint8_t> triangleResize(const uint8_t* srcRgba, int srcW, int srcH, int dstW, int dstH) {
      // image crate order: vertical first → Rgba32FImage(srcW × dstH)
      //                    horizontal      → RgbaImage(dstW × dstH)
      std::vector<float> tmp(static_cast<std::size_t>(srcW) * static_cast<std::size_t>(dstH) * 4U);
      verticalSampleU8ToF32(srcRgba, srcW, srcH, tmp.data(), dstH);
      std::vector<uint8_t> dst(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH) * 4U);
      horizontalSampleF32ToU8(tmp.data(), srcW, dstH, dst.data(), dstW);
      return dst;
    }

    std::vector<uint8_t> boxResize(const uint8_t* srcRgba, int srcW, int srcH, int dstW, int dstH) {
      std::vector<float> tmp(static_cast<std::size_t>(srcW) * static_cast<std::size_t>(dstH) * 4U);
      verticalBoxSampleU8ToF32(srcRgba, srcW, srcH, tmp.data(), dstH);
      std::vector<uint8_t> dst(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH) * 4U);
      horizontalBoxSampleF32ToU8(tmp.data(), srcW, dstH, dst.data(), dstW);
      return dst;
    }

  } // namespace

  std::expected<LoadedImage, std::string> loadAndResize(std::string_view path, Scheme scheme) {
    auto bytes = readFile(path);
    if (!bytes) {
      return std::unexpected(bytes.error());
    }
    if (bytes->empty()) {
      return std::unexpected("empty image file");
    }

    auto decoded = decodeRasterImage(bytes->data(), bytes->size());
    if (!decoded) {
      return std::unexpected(decoded.error());
    }

    const int srcW = decoded->width;
    const int srcH = decoded->height;
    if (srcW <= 0 || srcH <= 0) {
      return std::unexpected("invalid image dimensions");
    }

    // Force-resize to 112×112 (aspect ratio ignored).
    std::vector<uint8_t> resizedRgba = isMaterialScheme(scheme)
        ? triangleResize(decoded->pixels.data(), srcW, srcH, kTarget, kTarget)
        : boxResize(decoded->pixels.data(), srcW, srcH, kTarget, kTarget);

    LoadedImage out;
    out.rgb.resize(kTarget * kTarget * 3);
    for (int i = 0; i < kTarget * kTarget; ++i) {
      const auto rgbIndex = static_cast<std::size_t>(i) * 3U;
      const auto rgbaIndex = static_cast<std::size_t>(i) * 4U;
      out.rgb[rgbIndex + 0U] = resizedRgba[rgbaIndex + 0U];
      out.rgb[rgbIndex + 1U] = resizedRgba[rgbaIndex + 1U];
      out.rgb[rgbIndex + 2U] = resizedRgba[rgbaIndex + 2U];
    }
    return out;
  }

} // namespace noctalia::theme
