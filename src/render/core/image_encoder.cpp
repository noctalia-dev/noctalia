#include "render/core/image_encoder.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <cstring>

std::vector<std::uint8_t> encodePng(const std::uint8_t* rgba, int width, int height, std::string* errorOut) {
  if (rgba == nullptr || width <= 0 || height <= 0) {
    if (errorOut != nullptr) {
      *errorOut = "invalid image";
    }
    return {};
  }

  int encodedLength = 0;
  unsigned char* encoded = stbi_write_png_to_mem(rgba, width * 4, width, height, 4, &encodedLength);
  if (encoded == nullptr || encodedLength <= 0) {
    if (errorOut != nullptr) {
      *errorOut = "PNG encode failed";
    }
    return {};
  }

  std::vector<std::uint8_t> out(static_cast<std::size_t>(encodedLength));
  std::memcpy(out.data(), encoded, out.size());
  STBIW_FREE(encoded);
  return out;
}
