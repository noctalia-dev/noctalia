#pragma once

#include "render/core/image_file_loader.h"

namespace shell::dock {

  constexpr int kIconShadowPadding = 6;
  constexpr int kIconShadowOffsetY = 2;

  [[nodiscard]] LoadedImageFile makeIconShadow(const LoadedImageFile& source, int iconSize, int density);

} // namespace shell::dock
