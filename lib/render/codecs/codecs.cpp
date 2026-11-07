#include "render/codecs/codecs.h"

#include <memory>

namespace codecs {

  namespace {

    class CodecPackDecoder final : public ImageDecoder {
    public:
      std::expected<DecodedRasterImage, std::string> decode(const std::uint8_t* data, std::size_t size) override {
        return codecs::decodeRasterImage(data, size);
      }

      std::expected<LoadedImageFile, std::string>
      loadFile(const std::string& path, int targetSize, bool centerSquareCrop) override {
        return codecs::loadImageFile(path, targetSize, centerSquareCrop);
      }
    };

  } // namespace

  void installImageDecoder() { setImageDecoder(std::make_unique<CodecPackDecoder>()); }

} // namespace codecs
