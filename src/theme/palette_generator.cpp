#include "theme/palette_generator.h"

namespace noctalia::theme {

  GeneratedPalette generate(
      const std::vector<uint8_t>& rgb112, const std::vector<uint32_t>& fallback_colors, Scheme scheme,
      std::string* errorMessage
  ) {
    if (rgb112.size() != 112u * 112u * 3u) {
      if (errorMessage)
        *errorMessage = "expected 112x112x3 pixel buffer";
      return {};
    }
    if (isMaterialScheme(scheme))
      return generateMaterial(rgb112, fallback_colors, scheme);
    return generateCustom(rgb112, scheme);
  }

} // namespace noctalia::theme
