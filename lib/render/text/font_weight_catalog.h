#pragma once

#include "render/core/renderer.h"

#include <string_view>
#include <vector>

namespace text {

  void invalidateFontWeightCatalogCache();

  bool fontFamilySupportsVariableWeight(std::string_view fontFamily);

  std::vector<FontWeight> availableLabelFontWeights(std::string_view fontFamily);

} // namespace text
