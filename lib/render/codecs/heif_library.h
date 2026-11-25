#pragma once

#include "render/core/image_decoder.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

// Internal to the codec pack.
namespace codecs {

  // True when maxPixels > 0 and width * height exceeds it (overflow safe).
  inline bool exceedsMaxPixels(std::uint64_t width, std::uint64_t height, std::size_t maxPixels) {
    return maxPixels > 0 && height != 0 && width > maxPixels / height;
  }

  // Returns true if the buffer is an ISOBMFF file whose `ftyp` box names a HEIF or AVIF brand.
  bool isHeifContainer(const std::uint8_t* data, std::size_t size);

  // Decodes the primary image of a HEIF/AVIF file to straight-alpha RGBA8 with the container's
  // rotation/mirror/crop transforms applied. libheif is loaded with dlopen on first use and is not
  // a build or link dependency; when it is missing the call fails with a descriptive error.
  // maxPixels > 0 rejects images whose declared width * height exceeds it before decoding.
  std::expected<DecodedRasterImage, std::string>
  decodeHeif(const std::uint8_t* data, std::size_t size, std::size_t maxPixels);

} // namespace codecs
