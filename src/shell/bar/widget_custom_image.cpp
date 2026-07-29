#include "shell/bar/widget_custom_image.h"

#include "util/file_utils.h"

namespace widget_custom_image {

  WidgetCustomImage fromConfig(const std::string& path, bool colorize) {
    return {
        .path = FileUtils::expandUserPath(path).string(),
        .colorize = colorize,
    };
  }

} // namespace widget_custom_image
