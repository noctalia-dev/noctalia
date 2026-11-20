#pragma once

#include "render/core/image_decoder.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace codecs {

  // Rasterizes SVG bytes with librsvg; targetSize > 0 bounds the longer edge. A non-empty `stylesheet` is applied as
  // librsvg's user stylesheet.
  std::expected<LoadedImageFile, std::string>
  rasterizeSvg(std::span<const std::uint8_t> data, int targetSize, std::string_view stylesheet);

} // namespace codecs
