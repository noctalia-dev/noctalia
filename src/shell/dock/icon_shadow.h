#pragma once

#include "render/core/image_file_loader.h"

namespace shell::dock {

  [[nodiscard]] LoadedImageFile makeIconShadow(const LoadedImageFile& source, int iconSize, int density);

} // namespace shell::dock
