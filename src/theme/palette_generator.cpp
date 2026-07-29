#include "theme/palette_generator.h"

#include "theme/scheme.h"

#include <utility>

namespace noctalia::theme {

  namespace {

    std::expected<GeneratedPalette, std::string> validateGeneratedPalette(GeneratedPalette palette) {
      if (palette.dark.empty() && palette.light.empty()) {
        return std::unexpected("palette generation produced no token maps");
      }
      if (palette.dark.empty()) {
        return std::unexpected("palette generation produced an empty dark token map");
      }
      if (palette.light.empty()) {
        return std::unexpected("palette generation produced an empty light token map");
      }
      return palette;
    }

  } // namespace

  std::expected<GeneratedPalette, std::string> generate(const std::vector<uint8_t>& rgb112, Scheme scheme) {
    if (rgb112.size() != 112U * 112U * 3U) {
      return std::unexpected("expected 112x112x3 pixel buffer");
    }

    auto palette = isMaterialScheme(scheme) ? generateMaterial(rgb112, scheme) : generateCustom(rgb112, scheme);
    return validateGeneratedPalette(std::move(palette));
  }

} // namespace noctalia::theme
