#include "render/core/image_file_loader.h"

#include "render/core/image_decoder.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <cairo.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stb_image_resize2.h>
#include <string_view>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wexpansion-to-defined"
#include <librsvg/rsvg.h>
#pragma GCC diagnostic pop

namespace {

  // Convert a cairo ARGB32 image surface (premultiplied BGRA on little-endian)
  // into the non-premultiplied RGBA buffer the rest of the pipeline expects.
  void argb32ToRgba(const unsigned char* src, int srcStride, std::uint8_t* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
      const std::uint32_t* row = reinterpret_cast<const std::uint32_t*>(src + (y * srcStride));
      std::uint8_t* outRow = dst + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U);
      for (int x = 0; x < width; ++x) {
        const std::uint32_t pixel = row[x];
        const std::uint8_t a = static_cast<std::uint8_t>((pixel >> 24) & 0xFF);
        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xFF);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xFF);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xFF);
        if (a != 0 && a != 255) {
          // Un-premultiply, rounding to nearest.
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

  std::optional<LoadedImageFile>
  rasterizeSvg(const std::vector<std::uint8_t>& fileData, int targetSize, std::string* errorMessage);

  [[nodiscard]] bool asciiStartsWithDataScheme(std::string_view source) {
    if (source.size() < 5) {
      return false;
    }
    return (source[0] == 'd' || source[0] == 'D') && (source[1] == 'a' || source[1] == 'A') &&
           (source[2] == 't' || source[2] == 'T') && (source[3] == 'a' || source[3] == 'A') && source[4] == ':';
  }

  [[nodiscard]] char asciiLower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

  [[nodiscard]] bool asciiEqualInsensitive(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      if (asciiLower(lhs[i]) != asciiLower(rhs[i])) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool asciiWhitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f';
  }

  [[nodiscard]] std::string_view trimAscii(std::string_view value) {
    while (!value.empty() && asciiWhitespace(value.front())) {
      value.remove_prefix(1);
    }
    while (!value.empty() && asciiWhitespace(value.back())) {
      value.remove_suffix(1);
    }
    return value;
  }

  [[nodiscard]] int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
      return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
      return ch - 'A' + 10;
    }
    return -1;
  }

  [[nodiscard]] std::optional<std::vector<std::uint8_t>>
  percentDecode(std::string_view value, std::string* errorMessage) {
    std::vector<std::uint8_t> out;
    out.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
      const char ch = value[i];
      if (ch != '%') {
        out.push_back(static_cast<std::uint8_t>(ch));
        continue;
      }

      if (i + 2 >= value.size()) {
        if (errorMessage != nullptr) {
          *errorMessage = "data URI has truncated percent escape";
        }
        return std::nullopt;
      }

      const int hi = hexValue(value[i + 1]);
      const int lo = hexValue(value[i + 2]);
      if (hi < 0 || lo < 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "data URI has invalid percent escape";
        }
        return std::nullopt;
      }

      out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
      i += 2;
    }

    return out;
  }

  [[nodiscard]] int base64Value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
      return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
      return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
      return ch - '0' + 52;
    }
    if (ch == '+') {
      return 62;
    }
    if (ch == '/') {
      return 63;
    }
    return -1;
  }

  [[nodiscard]] std::optional<std::vector<std::uint8_t>>
  base64Decode(std::string_view value, std::string* errorMessage) {
    std::vector<std::uint8_t> out;
    out.reserve((value.size() * 3U) / 4U);

    std::array<int, 4> quad{};
    int quadSize = 0;
    bool finished = false;

    auto fail = [&](const char* message) -> std::optional<std::vector<std::uint8_t>> {
      if (errorMessage != nullptr) {
        *errorMessage = message;
      }
      return std::nullopt;
    };

    auto emitQuad = [&]() -> bool {
      if (quad[0] < 0 || quad[1] < 0) {
        return false;
      }

      out.push_back(static_cast<std::uint8_t>((quad[0] << 2) | (quad[1] >> 4)));
      if (quad[2] == -2) {
        return quad[3] == -2;
      }
      if (quad[2] < 0) {
        return false;
      }

      out.push_back(static_cast<std::uint8_t>(((quad[1] & 0x0F) << 4) | (quad[2] >> 2)));
      if (quad[3] == -2) {
        return true;
      }
      if (quad[3] < 0) {
        return false;
      }

      out.push_back(static_cast<std::uint8_t>(((quad[2] & 0x03) << 6) | quad[3]));
      return true;
    };

    for (char rawCh : value) {
      if (asciiWhitespace(rawCh)) {
        continue;
      }
      if (finished) {
        return fail("data URI has trailing base64 data after padding");
      }

      int decoded = -1;
      const auto ch = static_cast<unsigned char>(rawCh);
      if (ch == '=') {
        decoded = -2;
      } else {
        decoded = base64Value(ch);
        if (decoded < 0) {
          return fail("data URI has invalid base64 data");
        }
      }

      quad[static_cast<std::size_t>(quadSize++)] = decoded;
      if (quadSize == 4) {
        if (!emitQuad()) {
          return fail("data URI has invalid base64 padding");
        }
        finished = quad[2] == -2 || quad[3] == -2;
        quadSize = 0;
      }
    }

    if (quadSize == 0) {
      return out;
    }
    if (finished || quadSize == 1) {
      return fail("data URI has incomplete base64 data");
    }
    if (quad[0] < 0 || quad[1] < 0) {
      return fail("data URI has invalid base64 padding");
    }

    out.push_back(static_cast<std::uint8_t>((quad[0] << 2) | (quad[1] >> 4)));
    if (quadSize == 3) {
      if (quad[2] < 0) {
        return fail("data URI has invalid base64 padding");
      }
      out.push_back(static_cast<std::uint8_t>(((quad[1] & 0x0F) << 4) | (quad[2] >> 2)));
    }
    return out;
  }

  struct DecodedDataUri {
    std::vector<std::uint8_t> bytes;
    bool declaredSvg = false;
  };

  [[nodiscard]] std::optional<DecodedDataUri> decodeDataUri(std::string_view source, std::string* errorMessage) {
    const std::size_t comma = source.find(',');
    if (comma == std::string_view::npos) {
      if (errorMessage != nullptr) {
        *errorMessage = "data URI is missing payload separator";
      }
      return std::nullopt;
    }

    const std::string_view header = source.substr(5, comma - 5);
    const std::string_view payload = source.substr(comma + 1);

    bool base64 = false;
    bool declaredSvg = false;
    std::size_t tokenStart = 0;
    bool firstToken = true;
    while (tokenStart <= header.size()) {
      const std::size_t tokenEnd = header.find(';', tokenStart);
      const std::string_view token = trimAscii(
          header.substr(tokenStart, tokenEnd == std::string_view::npos ? std::string_view::npos : tokenEnd - tokenStart)
      );
      if (firstToken && asciiEqualInsensitive(token, "image/svg+xml")) {
        declaredSvg = true;
      } else if (asciiEqualInsensitive(token, "base64")) {
        base64 = true;
      }

      if (tokenEnd == std::string_view::npos) {
        break;
      }
      tokenStart = tokenEnd + 1;
      firstToken = false;
    }

    auto decodedPayload = percentDecode(payload, errorMessage);
    if (!decodedPayload.has_value()) {
      return std::nullopt;
    }

    if (!base64) {
      return DecodedDataUri{.bytes = std::move(*decodedPayload), .declaredSvg = declaredSvg};
    }

    std::string decodedText;
    decodedText.reserve(decodedPayload->size());
    for (std::uint8_t byte : *decodedPayload) {
      decodedText.push_back(static_cast<char>(byte));
    }

    auto decodedBytes = base64Decode(decodedText, errorMessage);
    if (!decodedBytes.has_value()) {
      return std::nullopt;
    }
    return DecodedDataUri{.bytes = std::move(*decodedBytes), .declaredSvg = declaredSvg};
  }

  [[nodiscard]] bool startsWithUtf8Bom(const std::vector<std::uint8_t>& data) {
    return data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF;
  }

  [[nodiscard]] bool looksLikeSvg(const std::vector<std::uint8_t>& data) {
    std::size_t pos = startsWithUtf8Bom(data) ? 3U : 0U;
    while (pos < data.size() && asciiWhitespace(static_cast<char>(data[pos]))) {
      ++pos;
    }

    const std::size_t scanEnd = std::min(data.size(), pos + 256U);
    for (; pos < scanEnd; ++pos) {
      if (pos + 4 <= data.size() && data[pos] == '<' && asciiLower(static_cast<char>(data[pos + 1])) == 's' &&
          asciiLower(static_cast<char>(data[pos + 2])) == 'v' && asciiLower(static_cast<char>(data[pos + 3])) == 'g') {
        return true;
      }
    }
    return false;
  }

  std::optional<LoadedImageFile>
  loadImageBytes(std::vector<std::uint8_t> fileData, bool preferSvg, int targetSize, std::string* errorMessage) {
    if (fileData.empty()) {
      if (errorMessage != nullptr) {
        *errorMessage = "empty image data";
      }
      return std::nullopt;
    }

    const bool svgLike = looksLikeSvg(fileData);
    if (preferSvg || svgLike) {
      if (auto loaded = rasterizeSvg(fileData, targetSize, errorMessage)) {
        return loaded;
      }
      if (svgLike) {
        return std::nullopt;
      }
    }

    if (auto decoded = decodeRasterImage(fileData.data(), fileData.size(), errorMessage)) {
      int width = decoded->width;
      int height = decoded->height;
      auto pixels = std::move(decoded->pixels);

      const int maxDim = std::max(width, height);
      if (targetSize > 0 && maxDim > targetSize && width > 0 && height > 0) {
        const float scale = static_cast<float>(targetSize) / static_cast<float>(maxDim);
        const int resizedW = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale)));
        const int resizedH = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale)));

        std::vector<std::uint8_t> resized(static_cast<std::size_t>(resizedW) * static_cast<std::size_t>(resizedH) * 4U);
        // Use the sRGB resize: image bytes are sRGB-encoded, so averaging them
        // directly (the _linear variant) darkens and muddies downscaled icons.
        // STBIR_RGBA handles the non-premultiplied alpha correctly.
        unsigned char* result =
            stbir_resize_uint8_srgb(pixels.data(), width, height, 0, resized.data(), resizedW, resizedH, 0, STBIR_RGBA);
        if (result != nullptr) {
          pixels = std::move(resized);
          width = resizedW;
          height = resizedH;
        }
      }

      return LoadedImageFile{.rgba = std::move(pixels), .width = width, .height = height};
    }

    return std::nullopt;
  }

  std::optional<LoadedImageFile>
  rasterizeSvg(const std::vector<std::uint8_t>& fileData, int targetSize, std::string* errorMessage) {
    GError* gerror = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_data(fileData.data(), fileData.size(), &gerror);
    if (handle == nullptr) {
      if (errorMessage != nullptr) {
        *errorMessage = std::string("failed to parse SVG: ") + (gerror != nullptr ? gerror->message : "unknown");
      }
      if (gerror != nullptr) {
        g_error_free(gerror);
      }
      return std::nullopt;
    }

    // Determine intrinsic pixel size. Many real-world SVGs (e.g. viewBox-only)
    // do not advertise pixel dimensions, so fall back to the viewBox or to a
    // sensible default before computing the render scale.
    gdouble intrinsicW = 0.0;
    gdouble intrinsicH = 0.0;
    gboolean hasIntrinsic = rsvg_handle_get_intrinsic_size_in_pixels(handle, &intrinsicW, &intrinsicH);
    if (hasIntrinsic == FALSE || intrinsicW <= 0.0 || intrinsicH <= 0.0) {
      gboolean outHasW = FALSE;
      RsvgLength outW{};
      gboolean outHasH = FALSE;
      RsvgLength outH{};
      gboolean outHasViewbox = FALSE;
      RsvgRectangle outViewbox{};
      rsvg_handle_get_intrinsic_dimensions(handle, &outHasW, &outW, &outHasH, &outH, &outHasViewbox, &outViewbox);
      if (outHasViewbox == TRUE && outViewbox.width > 0.0 && outViewbox.height > 0.0) {
        intrinsicW = outViewbox.width;
        intrinsicH = outViewbox.height;
      } else {
        intrinsicW = 512.0;
        intrinsicH = 512.0;
      }
    }

    int width = static_cast<int>(std::round(intrinsicW));
    int height = static_cast<int>(std::round(intrinsicH));
    if (targetSize > 0) {
      const double maxSide = std::max(intrinsicW, intrinsicH);
      const double scale = static_cast<double>(targetSize) / maxSide;
      width = std::max(1, static_cast<int>(std::round(intrinsicW * scale)));
      height = std::max(1, static_cast<int>(std::round(intrinsicH * scale)));
    }
    if (width <= 0 || height <= 0) {
      if (errorMessage != nullptr) {
        *errorMessage = "invalid SVG dimensions";
      }
      g_object_unref(handle);
      return std::nullopt;
    }

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      if (errorMessage != nullptr) {
        *errorMessage = "failed to create cairo surface";
      }
      cairo_surface_destroy(surface);
      g_object_unref(handle);
      return std::nullopt;
    }

    cairo_t* cr = cairo_create(surface);
    RsvgRectangle viewport{
        .x = 0.0,
        .y = 0.0,
        .width = static_cast<double>(width),
        .height = static_cast<double>(height),
    };
    GError* renderError = nullptr;
    if (rsvg_handle_render_document(handle, cr, &viewport, &renderError) == FALSE) {
      if (errorMessage != nullptr) {
        *errorMessage =
            std::string("failed to render SVG: ") + (renderError != nullptr ? renderError->message : "unknown");
      }
      if (renderError != nullptr) {
        g_error_free(renderError);
      }
      cairo_destroy(cr);
      cairo_surface_destroy(surface);
      g_object_unref(handle);
      return std::nullopt;
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    LoadedImageFile loaded{
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U),
        .width = width,
        .height = height,
    };
    argb32ToRgba(
        cairo_image_surface_get_data(surface), cairo_image_surface_get_stride(surface), loaded.rgba.data(), width,
        height
    );

    cairo_surface_destroy(surface);
    g_object_unref(handle);
    return loaded;
  }

} // namespace

std::optional<LoadedImageFile> loadImageFile(const std::string& path, int targetSize, std::string* errorMessage) {
  if (path.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "empty image path";
    }
    return std::nullopt;
  }

  if (asciiStartsWithDataScheme(path)) {
    auto dataUri = decodeDataUri(path, errorMessage);
    if (!dataUri.has_value()) {
      return std::nullopt;
    }
    return loadImageBytes(std::move(dataUri->bytes), dataUri->declaredSvg, targetSize, errorMessage);
  }

  auto fileData = FileUtils::readBinaryFile(path);
  if (fileData.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "failed to read image file";
    }
    return std::nullopt;
  }

  return loadImageBytes(
      std::move(fileData), path.ends_with(".svg") || path.ends_with(".SVG"), targetSize, errorMessage
  );
}
