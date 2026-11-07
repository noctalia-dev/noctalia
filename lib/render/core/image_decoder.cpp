#include "render/core/image_decoder.h"

#include <utility>

namespace {

  std::unique_ptr<ImageDecoder>& installedDecoder() {
    static std::unique_ptr<ImageDecoder> decoder;
    return decoder;
  }

  constexpr const char* kNoDecoder = "no image decoder installed";

} // namespace

void setImageDecoder(std::unique_ptr<ImageDecoder> decoder) { installedDecoder() = std::move(decoder); }

std::expected<DecodedRasterImage, std::string> decodeRasterImage(const std::uint8_t* data, std::size_t size) {
  if (installedDecoder() == nullptr) {
    return std::unexpected(kNoDecoder);
  }
  return installedDecoder()->decode(data, size);
}

std::expected<LoadedImageFile, std::string>
loadImageFile(const std::string& path, int targetSize, bool centerSquareCrop) {
  if (installedDecoder() == nullptr) {
    return std::unexpected(kNoDecoder);
  }
  return installedDecoder()->loadFile(path, targetSize, centerSquareCrop);
}
