#pragma once

#include "render/core/image_decoder.h"

namespace shell::dock {

  [[nodiscard]] LoadedImageFile makeIconShadow(const LoadedImageFile& source, int iconSize, int density);

} // namespace shell::dock
