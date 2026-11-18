#include "render/codecs/codecs.h"
#include "render/codecs/icon_color_scheme.h"
#include "render/codecs/svg_rasterizer.h"
#include "render/core/image_resize.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <string_view>
#include <utility>

namespace codecs {

  namespace {

    bool asciiStartsWithDataScheme(std::string_view source) {
      if (source.size() < 5) {
        return false;
      }
      return (source[0] == 'd' || source[0] == 'D')
          && (source[1] == 'a' || source[1] == 'A')
          && (source[2] == 't' || source[2] == 'T')
          && (source[3] == 'a' || source[3] == 'A')
          && source[4] == ':';
    }

    char asciiLower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

    bool asciiEqualInsensitive(std::string_view lhs, std::string_view rhs) {
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

    bool asciiWhitespace(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f'; }

    std::string_view trimAscii(std::string_view value) {
      while (!value.empty() && asciiWhitespace(value.front())) {
        value.remove_prefix(1);
      }
      while (!value.empty() && asciiWhitespace(value.back())) {
        value.remove_suffix(1);
      }
      return value;
    }

    int hexValue(char ch) {
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

    std::expected<std::vector<std::uint8_t>, std::string> percentDecode(std::string_view value) {
      std::vector<std::uint8_t> out;
      out.reserve(value.size());

      for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch != '%') {
          out.push_back(static_cast<std::uint8_t>(ch));
          continue;
        }

        if (i + 2 >= value.size()) {
          return std::unexpected("data URI has truncated percent escape");
        }

        const int hi = hexValue(value[i + 1]);
        const int lo = hexValue(value[i + 2]);
        if (hi < 0 || lo < 0) {
          return std::unexpected("data URI has invalid percent escape");
        }

        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        i += 2;
      }

      return out;
    }

    int base64Value(unsigned char ch) {
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

    std::expected<std::vector<std::uint8_t>, std::string> base64Decode(std::string_view value) {
      std::vector<std::uint8_t> out;
      out.reserve((value.size() * 3U) / 4U);

      std::array<int, 4> quad{};
      int quadSize = 0;
      bool finished = false;

      auto fail = [](const char* message) -> std::expected<std::vector<std::uint8_t>, std::string> {
        return std::unexpected(message);
      };

      auto emitQuad = [&] -> bool {
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

    std::expected<DecodedDataUri, std::string> decodeDataUri(std::string_view source) {
      const std::size_t comma = source.find(',');
      if (comma == std::string_view::npos) {
        return std::unexpected("data URI is missing payload separator");
      }

      const std::string_view header = source.substr(5, comma - 5);
      const std::string_view payload = source.substr(comma + 1);

      bool base64 = false;
      bool declaredSvg = false;
      std::size_t tokenStart = 0;
      bool firstToken = true;
      while (tokenStart <= header.size()) {
        const std::size_t tokenEnd = header.find(';', tokenStart);
        const std::string_view token = trimAscii(header.substr(
            tokenStart, tokenEnd == std::string_view::npos ? std::string_view::npos : tokenEnd - tokenStart
        ));
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

      auto decodedPayload = percentDecode(payload);
      if (!decodedPayload) {
        return std::unexpected(decodedPayload.error());
      }

      if (!base64) {
        return DecodedDataUri{.bytes = std::move(*decodedPayload), .declaredSvg = declaredSvg};
      }

      std::string decodedText;
      decodedText.reserve(decodedPayload->size());
      for (std::uint8_t byte : *decodedPayload) {
        decodedText.push_back(static_cast<char>(byte));
      }

      auto decodedBytes = base64Decode(decodedText);
      if (!decodedBytes) {
        return std::unexpected(decodedBytes.error());
      }
      return DecodedDataUri{.bytes = std::move(*decodedBytes), .declaredSvg = declaredSvg};
    }

    bool startsWithUtf8Bom(const std::vector<std::uint8_t>& data) {
      return data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF;
    }

    bool looksLikeSvg(const std::vector<std::uint8_t>& data) {
      std::size_t pos = startsWithUtf8Bom(data) ? 3U : 0U;
      while (pos < data.size() && asciiWhitespace(static_cast<char>(data[pos]))) {
        ++pos;
      }

      const std::size_t scanEnd = std::min(data.size(), pos + 256U);
      for (; pos < scanEnd; ++pos) {
        if (pos + 4 <= data.size()
            && data[pos] == '<'
            && asciiLower(static_cast<char>(data[pos + 1])) == 's'
            && asciiLower(static_cast<char>(data[pos + 2])) == 'v'
            && asciiLower(static_cast<char>(data[pos + 3])) == 'g') {
          return true;
        }
      }
      return false;
    }

    LoadedImageFile cropCenterSquare(LoadedImageFile image) {
      const int side = std::min(image.width, image.height);
      if (side <= 0 || (image.width == side && image.height == side)) {
        return image;
      }

      const int offsetX = (image.width - side) / 2;
      const int offsetY = (image.height - side) / 2;
      std::vector<std::uint8_t> cropped(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4U);
      for (int row = 0; row < side; ++row) {
        const auto srcRow = static_cast<std::size_t>(offsetY + row) * static_cast<std::size_t>(image.width) * 4U
            + static_cast<std::size_t>(offsetX) * 4U;
        const auto dstRow = static_cast<std::size_t>(row) * static_cast<std::size_t>(side) * 4U;
        std::copy_n(image.rgba.data() + srcRow, static_cast<std::size_t>(side) * 4U, cropped.data() + dstRow);
      }

      image.rgba = std::move(cropped);
      image.width = side;
      image.height = side;
      return image;
    }

    std::expected<LoadedImageFile, std::string> loadImageBytes(
        std::vector<std::uint8_t> fileData, bool preferSvg, int targetSize, bool centerSquareCrop,
        std::string_view stylesheet
    ) {
      if (fileData.empty()) {
        return std::unexpected("empty image data");
      }

      const bool svgLike = looksLikeSvg(fileData);
      if (preferSvg || svgLike) {
        auto loaded = rasterizeSvg(fileData, targetSize, stylesheet);
        if (loaded && centerSquareCrop) {
          return cropCenterSquare(std::move(*loaded));
        }
        if (loaded) {
          return loaded;
        }
        if (svgLike) {
          return loaded;
        }
      }

      auto decoded = decodeRasterImage(fileData.data(), fileData.size());
      if (!decoded) {
        return std::unexpected(decoded.error());
      }

      LoadedImageFile loaded{
          .rgba = std::move(decoded->pixels),
          .width = decoded->width,
          .height = decoded->height,
          .sourceWidth = decoded->width,
          .sourceHeight = decoded->height,
      };

      // Crop before resizing so the kept square fills targetSize at full detail,
      // instead of resizing the whole frame and discarding most of it afterwards.
      if (centerSquareCrop) {
        loaded = cropCenterSquare(std::move(loaded));
      }

      if (targetSize > 0) {
        const ImageExtent fitted = fitImageWithin(loaded.width, loaded.height, targetSize, targetSize);
        if (fitted.width != loaded.width || fitted.height != loaded.height) {
          auto resized = resizeToRgba(
              loaded.rgba.data(), loaded.width, loaded.height, ResizeSourceLayout::Rgba, fitted.width, fitted.height
          );
          if (!resized.empty()) {
            loaded.rgba = std::move(resized);
            loaded.width = fitted.width;
            loaded.height = fitted.height;
          }
        }
      }

      return loaded;
    }

  } // namespace

  std::expected<LoadedImageFile, std::string>
  loadImageFile(const std::string& path, int targetSize, bool centerSquareCrop) {
    if (path.empty()) {
      return std::unexpected("empty image path");
    }

    if (asciiStartsWithDataScheme(path)) {
      return decodeDataUri(path).and_then([&](DecodedDataUri dataUri) {
        return loadImageBytes(std::move(dataUri.bytes), dataUri.declaredSvg, targetSize, centerSquareCrop, {});
      });
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
      return std::unexpected("path is not a regular file");
    }

    auto fileData = FileUtils::readBinaryFile(path);
    if (fileData.empty()) {
      return std::unexpected("failed to read image file");
    }

    const auto stylesheet = iconColorSchemeStylesheetFor(path, fileData);
    return loadImageBytes(
        std::move(fileData), path.ends_with(".svg") || path.ends_with(".SVG"), targetSize, centerSquareCrop,
        stylesheet != nullptr ? std::string_view(*stylesheet) : std::string_view()
    );
  }

} // namespace codecs
