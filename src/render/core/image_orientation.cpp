#include "render/core/image_orientation.h"

#include <array>
#include <cstring>
#include <span>
#include <utility>

namespace {

  constexpr std::uint16_t kOrientationTag = 0x0112;
  constexpr std::uint16_t kTypeShort = 3;
  constexpr std::uint16_t kTypeLong = 4;
  constexpr std::size_t kTiffHeaderSize = 8;
  constexpr std::size_t kTiffEntrySize = 12;
  constexpr std::array<std::uint8_t, 6> kExifPrefix{'E', 'x', 'i', 'f', 0, 0};
  constexpr std::array<std::uint8_t, 8> kPngSignature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

  [[nodiscard]] std::uint16_t readU16(const std::uint8_t* p, bool bigEndian) {
    return bigEndian ? static_cast<std::uint16_t>((p[0] << 8) | p[1]) : static_cast<std::uint16_t>(p[0] | (p[1] << 8));
  }

  [[nodiscard]] std::uint32_t readU32(const std::uint8_t* p, bool bigEndian) {
    const std::uint32_t b0 = p[0];
    const std::uint32_t b1 = p[1];
    const std::uint32_t b2 = p[2];
    const std::uint32_t b3 = p[3];
    return bigEndian ? (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3 : (b3 << 24U) | (b2 << 16U) | (b1 << 8U) | b0;
  }

  [[nodiscard]] bool startsWith(std::span<const std::uint8_t> data, std::span<const std::uint8_t> prefix) {
    return data.size() >= prefix.size() && std::memcmp(data.data(), prefix.data(), prefix.size()) == 0;
  }

  [[nodiscard]] bool chunkIs(const std::uint8_t* type, const char* name) { return std::memcmp(type, name, 4) == 0; }

  [[nodiscard]] ImageOrientation orientationFromValue(std::uint32_t value) {
    return value >= 1U && value <= 8U ? static_cast<ImageOrientation>(value) : ImageOrientation::Normal;
  }

  // TIFF header plus IFD0; only tag 0x0112 is read, sub-IFDs are irrelevant for it.
  [[nodiscard]] ImageOrientation orientationFromTiff(std::span<const std::uint8_t> tiff) {
    if (tiff.size() < kTiffHeaderSize) {
      return ImageOrientation::Normal;
    }
    bool bigEndian = false;
    if (tiff[0] == 'I' && tiff[1] == 'I') {
      bigEndian = false;
    } else if (tiff[0] == 'M' && tiff[1] == 'M') {
      bigEndian = true;
    } else {
      return ImageOrientation::Normal;
    }
    if (readU16(tiff.data() + 2, bigEndian) != 42U) {
      return ImageOrientation::Normal;
    }

    const std::size_t ifd = readU32(tiff.data() + 4, bigEndian);
    if (ifd < kTiffHeaderSize || ifd + 2 > tiff.size()) {
      return ImageOrientation::Normal;
    }
    const std::size_t entries = readU16(tiff.data() + ifd, bigEndian);
    for (std::size_t i = 0; i < entries; ++i) {
      const std::size_t entry = ifd + 2 + (i * kTiffEntrySize);
      if (entry + kTiffEntrySize > tiff.size()) {
        break;
      }
      if (readU16(tiff.data() + entry, bigEndian) != kOrientationTag) {
        continue;
      }
      // SHORT and LONG values are left-justified in the 4-byte value field.
      const std::uint16_t type = readU16(tiff.data() + entry + 2, bigEndian);
      if (type == kTypeShort) {
        return orientationFromValue(readU16(tiff.data() + entry + 8, bigEndian));
      }
      if (type == kTypeLong) {
        return orientationFromValue(readU32(tiff.data() + entry + 8, bigEndian));
      }
      return ImageOrientation::Normal;
    }
    return ImageOrientation::Normal;
  }

  // PNG and WebP carry the bare TIFF block; some writers prepend the JPEG "Exif\0\0" header.
  [[nodiscard]] ImageOrientation orientationFromExifPayload(std::span<const std::uint8_t> payload) {
    if (startsWith(payload, kExifPrefix)) {
      return orientationFromTiff(payload.subspan(kExifPrefix.size()));
    }
    return orientationFromTiff(payload);
  }

  [[nodiscard]] ImageOrientation jpegOrientation(std::span<const std::uint8_t> data) {
    std::size_t pos = 2;
    while (pos + 4 <= data.size()) {
      if (data[pos] != 0xFF) {
        return ImageOrientation::Normal;
      }
      const std::uint8_t marker = data[pos + 1];
      // Fill bytes and the standalone markers (SOI, EOI, TEM, RST0-7) carry no length.
      if (marker == 0xFF) {
        ++pos;
        continue;
      }
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
        pos += 2;
        continue;
      }
      // Entropy-coded data starts here; metadata behind it is not reachable by scanning.
      if (marker == 0xDA) {
        return ImageOrientation::Normal;
      }

      const std::size_t segmentLength = (static_cast<std::size_t>(data[pos + 2]) << 8U) | data[pos + 3];
      if (segmentLength < 2 || pos + 2 + segmentLength > data.size()) {
        return ImageOrientation::Normal;
      }
      if (marker == 0xE1) {
        const std::span<const std::uint8_t> payload = data.subspan(pos + 4, segmentLength - 2);
        if (startsWith(payload, kExifPrefix)) {
          return orientationFromTiff(payload.subspan(kExifPrefix.size()));
        }
      }
      pos += 2 + segmentLength;
    }
    return ImageOrientation::Normal;
  }

  [[nodiscard]] ImageOrientation pngOrientation(std::span<const std::uint8_t> data) {
    std::size_t pos = kPngSignature.size();
    while (pos + 12 <= data.size()) {
      const std::size_t length = readU32(data.data() + pos, true);
      if (length > data.size() - pos - 12) {
        return ImageOrientation::Normal;
      }
      const std::uint8_t* type = data.data() + pos + 4;
      if (chunkIs(type, "eXIf")) {
        return orientationFromExifPayload(data.subspan(pos + 8, length));
      }
      // eXIf is allowed after IDAT, but nothing beyond IEND is part of the image.
      if (chunkIs(type, "IEND")) {
        return ImageOrientation::Normal;
      }
      pos += 12 + length;
    }
    return ImageOrientation::Normal;
  }

  [[nodiscard]] ImageOrientation webpOrientation(std::span<const std::uint8_t> data) {
    std::size_t pos = 12;
    while (pos + 8 <= data.size()) {
      const std::size_t length = readU32(data.data() + pos + 4, false);
      if (length > data.size() - pos - 8) {
        return ImageOrientation::Normal;
      }
      if (chunkIs(data.data() + pos, "EXIF")) {
        return orientationFromExifPayload(data.subspan(pos + 8, length));
      }
      pos += 8 + length + (length & 1U);
    }
    return ImageOrientation::Normal;
  }

  [[nodiscard]] bool isJpeg(std::span<const std::uint8_t> data) {
    return data.size() >= 4 && data[0] == 0xFF && data[1] == 0xD8;
  }

  [[nodiscard]] bool isWebPContainer(std::span<const std::uint8_t> data) {
    return data.size() >= 12 && chunkIs(data.data(), "RIFF") && chunkIs(data.data() + 8, "WEBP");
  }

  // Destination index of source pixel (x, y) is base + x*stepX + y*stepY, in pixels.
  struct OrientationMapping {
    std::ptrdiff_t base = 0;
    std::ptrdiff_t stepX = 1;
    std::ptrdiff_t stepY = 0;
    bool swapAxes = false;
  };

  [[nodiscard]] OrientationMapping
  mappingFor(ImageOrientation orientation, std::ptrdiff_t width, std::ptrdiff_t height) {
    switch (orientation) {
    case ImageOrientation::MirrorHorizontal:
      return {.base = width - 1, .stepX = -1, .stepY = width, .swapAxes = false};
    case ImageOrientation::Rotate180:
      return {.base = (width * height) - 1, .stepX = -1, .stepY = -width, .swapAxes = false};
    case ImageOrientation::MirrorVertical:
      return {.base = width * (height - 1), .stepX = 1, .stepY = -width, .swapAxes = false};
    case ImageOrientation::Transpose:
      return {.base = 0, .stepX = height, .stepY = 1, .swapAxes = true};
    case ImageOrientation::Rotate90:
      return {.base = height - 1, .stepX = height, .stepY = -1, .swapAxes = true};
    case ImageOrientation::Transverse:
      return {.base = (width * height) - 1, .stepX = -height, .stepY = -1, .swapAxes = true};
    case ImageOrientation::Rotate270:
      return {.base = height * (width - 1), .stepX = -height, .stepY = 1, .swapAxes = true};
    case ImageOrientation::Normal:
      break;
    }
    return {};
  }

} // namespace

ImageOrientation exifOrientation(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return ImageOrientation::Normal;
  }
  const std::span<const std::uint8_t> bytes(data, size);
  if (isJpeg(bytes)) {
    return jpegOrientation(bytes);
  }
  if (startsWith(bytes, kPngSignature)) {
    return pngOrientation(bytes);
  }
  if (isWebPContainer(bytes)) {
    return webpOrientation(bytes);
  }
  return ImageOrientation::Normal;
}

void applyImageOrientation(std::vector<std::uint8_t>& rgba, int& width, int& height, ImageOrientation orientation) {
  if (orientation == ImageOrientation::Normal || width <= 0 || height <= 0) {
    return;
  }
  const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (rgba.size() != pixels * 4U) {
    return;
  }

  const OrientationMapping mapping = mappingFor(orientation, width, height);
  std::vector<std::uint8_t> rotated(rgba.size());
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* source = rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U);
    std::ptrdiff_t destination = mapping.base + (static_cast<std::ptrdiff_t>(y) * mapping.stepY);
    for (int x = 0; x < width; ++x) {
      std::memcpy(rotated.data() + (static_cast<std::size_t>(destination) * 4U), source, 4U);
      source += 4;
      destination += mapping.stepX;
    }
  }

  rgba = std::move(rotated);
  if (mapping.swapAxes) {
    std::swap(width, height);
  }
}
