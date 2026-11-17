#pragma once

#include "render/core/color.h"
#include "wayland/native_data_transfer.h"

#include <string_view>

namespace filemanager {

  [[nodiscard]] NativeDataTransfer::DragIconImage renderFileDragIcon(
      std::string_view label, bool folder, int scale, Color background, Color foreground, Color accent
  );

} // namespace filemanager
