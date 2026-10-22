#pragma once

#include "render/core/image_decoder.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

// Returns true if the buffer is an ISOBMFF file whose `ftyp` box names a HEIF or AVIF brand.
[[nodiscard]] bool isHeifContainer(const std::uint8_t* data, std::size_t size);

// Decodes the primary image of a HEIF/AVIF file to straight-alpha RGBA8 with the container's
// rotation/mirror/crop transforms applied. libheif is loaded with dlopen on first use and is not
// a build or link dependency; when it is missing the call fails with a descriptive error.
[[nodiscard]] std::expected<DecodedRasterImage, std::string> decodeHeif(const std::uint8_t* data, std::size_t size);
