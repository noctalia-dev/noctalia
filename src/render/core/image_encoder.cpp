#include "render/core/image_encoder.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <jxl/color_encoding.h>
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <jxl/types.h>
#include <utility>
#include <webp/encode.h>

namespace {

  constexpr int kLosslessQuality = 100;
  // JPEG stores dimensions as 16-bit fields.
  constexpr int kJpegMaxDimension = 65535;

  void setError(std::string* errorOut, std::string message) {
    if (errorOut != nullptr) {
      *errorOut = std::move(message);
    }
  }

  // Same mapping as libjxl's JxlEncoderDistanceFromQuality, which only exists from libjxl 0.9;
  // the decoder side builds against 0.7, so keep the encoder on the 0.7 API too.
  [[nodiscard]] float jxlDistanceFromQuality(int quality) {
    const float q = static_cast<float>(std::clamp(quality, 1, 100));
    if (q >= 30.0F) {
      return 0.1F + ((100.0F - q) * 0.09F);
    }
    return 6.4F + (std::pow(2.5F, (30.0F - q) / 5.0F) / 6.25F);
  }

  [[nodiscard]] bool validInput(const std::uint8_t* rgba, int width, int height, std::string* errorOut) {
    if (rgba == nullptr || width <= 0 || height <= 0) {
      setError(errorOut, "invalid image");
      return false;
    }
    return true;
  }

  void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
  }

} // namespace

std::vector<std::uint8_t> encodePng(const std::uint8_t* rgba, int width, int height, std::string* errorOut) {
  if (!validInput(rgba, width, height, errorOut)) {
    return {};
  }

  int encodedLength = 0;
  unsigned char* encoded = stbi_write_png_to_mem(rgba, width * 4, width, height, 4, &encodedLength);
  if (encoded == nullptr || encodedLength <= 0) {
    setError(errorOut, "PNG encode failed");
    return {};
  }

  std::vector<std::uint8_t> out(static_cast<std::size_t>(encodedLength));
  std::memcpy(out.data(), encoded, out.size());
  STBIW_FREE(encoded);
  return out;
}

std::vector<std::uint8_t>
encodeJpeg(const std::uint8_t* rgba, int width, int height, int quality, std::string* errorOut) {
  if (!validInput(rgba, width, height, errorOut)) {
    return {};
  }
  if (width > kJpegMaxDimension || height > kJpegMaxDimension) {
    setError(errorOut, "image too large for JPEG");
    return {};
  }

  // The stb JPEG writer reads the first three of `comp` channels, so RGBA input drops alpha without a copy.
  std::vector<std::uint8_t> out;
  if (stbi_write_jpg_to_func(appendBytes, &out, width, height, 4, rgba, std::clamp(quality, 1, 100)) == 0
      || out.empty()) {
    setError(errorOut, "JPEG encode failed");
    return {};
  }
  return out;
}

std::vector<std::uint8_t>
encodeWebp(const std::uint8_t* rgba, int width, int height, int quality, std::string* errorOut) {
  if (!validInput(rgba, width, height, errorOut)) {
    return {};
  }
  if (width > WEBP_MAX_DIMENSION || height > WEBP_MAX_DIMENSION) {
    setError(errorOut, "image too large for WebP");
    return {};
  }

  std::uint8_t* encoded = nullptr;
  const std::size_t encodedSize = quality >= kLosslessQuality
      ? WebPEncodeLosslessRGBA(rgba, width, height, width * 4, &encoded)
      : WebPEncodeRGBA(rgba, width, height, width * 4, static_cast<float>(std::clamp(quality, 1, 100)), &encoded);
  if (encoded == nullptr || encodedSize == 0) {
    WebPFree(encoded);
    setError(errorOut, "WebP encode failed");
    return {};
  }

  std::vector<std::uint8_t> out(encoded, encoded + encodedSize);
  WebPFree(encoded);
  return out;
}

std::vector<std::uint8_t>
encodeJxl(const std::uint8_t* rgba, int width, int height, int quality, std::string* errorOut) {
  if (!validInput(rgba, width, height, errorOut)) {
    return {};
  }

  auto encoder = JxlEncoderMake(nullptr);
  auto runner = JxlResizableParallelRunnerMake(nullptr);
  if (encoder == nullptr || runner == nullptr) {
    setError(errorOut, "libjxl: failed to create encoder");
    return {};
  }

  const auto xsize = static_cast<std::uint32_t>(width);
  const auto ysize = static_cast<std::uint32_t>(height);
  JxlResizableParallelRunnerSetThreads(runner.get(), JxlResizableParallelRunnerSuggestThreads(xsize, ysize));
  if (JxlEncoderSetParallelRunner(encoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_ENC_SUCCESS) {
    setError(errorOut, "libjxl: failed to configure encoder");
    return {};
  }

  const bool lossless = quality >= kLosslessQuality;

  JxlBasicInfo info;
  JxlEncoderInitBasicInfo(&info);
  info.xsize = xsize;
  info.ysize = ysize;
  info.bits_per_sample = 8;
  info.num_color_channels = 3;
  info.num_extra_channels = 1;
  info.alpha_bits = 8;
  // Lossless output must keep the original sRGB samples; lossy output encodes in XYB.
  info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;

  JxlColorEncoding color;
  JxlColorEncodingSetToSRGB(&color, JXL_FALSE);

  if (JxlEncoderSetBasicInfo(encoder.get(), &info) != JXL_ENC_SUCCESS
      || JxlEncoderSetColorEncoding(encoder.get(), &color) != JXL_ENC_SUCCESS) {
    setError(errorOut, "libjxl: failed to set image header");
    return {};
  }

  JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
  if (settings == nullptr) {
    setError(errorOut, "libjxl: failed to create frame settings");
    return {};
  }
  const JxlEncoderStatus frameStatus = lossless ? JxlEncoderSetFrameLossless(settings, JXL_TRUE)
                                                : JxlEncoderSetFrameDistance(settings, jxlDistanceFromQuality(quality));
  if (frameStatus != JXL_ENC_SUCCESS) {
    setError(errorOut, "libjxl: failed to set frame quality");
    return {};
  }

  constexpr JxlPixelFormat kFormat{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  const std::size_t pixelBytes = static_cast<std::size_t>(xsize) * ysize * 4;
  if (JxlEncoderAddImageFrame(settings, &kFormat, rgba, pixelBytes) != JXL_ENC_SUCCESS) {
    setError(errorOut, "libjxl: failed to add image frame");
    return {};
  }
  JxlEncoderCloseInput(encoder.get());

  std::vector<std::uint8_t> out(std::max<std::size_t>(pixelBytes / 8, 64 * 1024));
  std::uint8_t* next = out.data();
  std::size_t available = out.size();
  for (;;) {
    const JxlEncoderStatus status = JxlEncoderProcessOutput(encoder.get(), &next, &available);
    if (status == JXL_ENC_SUCCESS) {
      break;
    }
    if (status != JXL_ENC_NEED_MORE_OUTPUT) {
      setError(errorOut, "JPEG XL encode failed");
      return {};
    }
    const auto written = static_cast<std::size_t>(next - out.data());
    out.resize(out.size() * 2);
    next = out.data() + written;
    available = out.size() - written;
  }
  out.resize(static_cast<std::size_t>(next - out.data()));
  return out;
}
