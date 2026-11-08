#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Decoded pixels leave the codec pack in sRGB. Images that embed another RGB color space (Display P3
// photos and wallpapers, Adobe RGB, BT.2020 SDR) are converted here with lcms2.
namespace codecs {

  // Converts RGBA8 straight-alpha pixels described by the ICC profile `icc` to sRGB in place. Leaves
  // them as they are when the profile is empty, unreadable, not RGB, or sRGB (primaries and curves).
  void convertIccToSrgb(std::vector<std::uint8_t>& rgba, std::span<const std::uint8_t> icc);

  // The same for a color space given as ITU-T H.273 code points (HEIF/AVIF nclx). Leaves the pixels
  // as they are for sRGB (BT.709 primaries, an SDR curve), unknown code points and HDR transfer functions.
  void convertCicpToSrgb(std::vector<std::uint8_t>& rgba, int primaries, int transfer);

  // The ICC profile embedded in a JPEG's APP2 ICC_PROFILE segments, reassembled; empty when there is none.
  std::vector<std::uint8_t> jpegIccProfile(const std::uint8_t* data, std::size_t size);

} // namespace codecs
