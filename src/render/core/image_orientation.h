#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// EXIF Orientation tag values (TIFF 6.0 tag 0x0112), describing how stored rows and
// columns must be transformed to reach display order.
enum class ImageOrientation : std::uint8_t {
  Normal = 1,
  MirrorHorizontal = 2,
  Rotate180 = 3,
  MirrorVertical = 4,
  Transpose = 5,
  Rotate90 = 6,
  Transverse = 7,
  Rotate270 = 8,
};

// Orientation carried by a JPEG APP1 Exif segment, a PNG eXIf chunk, or a WebP EXIF
// chunk. Missing, unreadable, or out-of-range metadata reads as Normal.
[[nodiscard]] ImageOrientation exifOrientation(const std::uint8_t* data, std::size_t size);

// Transforms an RGBA8 buffer into display order, swapping width and height for the
// quarter-turn orientations. No-op for Normal or for a buffer that is not width*height*4.
void applyImageOrientation(std::vector<std::uint8_t>& rgba, int& width, int& height, ImageOrientation orientation);
