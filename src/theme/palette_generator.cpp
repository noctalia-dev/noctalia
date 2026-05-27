#include "theme/palette_generator.h"

namespace noctalia::theme {

  GeneratedPalette generate(const std::vector<uint8_t>& rgb112, Scheme scheme, std::string* errorMessage) {
    if (rgb112.size() != 112u * 112u * 3u) {
      if (errorMessage) {
        *errorMessage = "expected 112x112x3 pixel buffer";
      }
      return {};
    }
    if (isMaterialScheme(scheme)) {
      return generateMaterial(rgb112, scheme);
    }
    return generateCustom(rgb112, scheme);
  }

} // namespace noctalia::theme
