#pragma once

#include "theme/community_palettes.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::theme {

  struct GeneratedPalette;

  std::filesystem::path customPaletteDir();
  std::filesystem::path customPalettePath(std::string_view name);
  std::vector<AvailablePalette> availableCustomPalettes();

  std::string suggestCustomPaletteName(std::string_view wallpaperPath, std::string_view scheme);
  std::string allocateCustomPaletteName(std::string_view wallpaperPath, std::string_view scheme);
  [[nodiscard]] bool saveCustomPaletteFromGenerated(
      std::string_view name, const GeneratedPalette& palette, std::string* errorOut = nullptr
  );

} // namespace noctalia::theme
