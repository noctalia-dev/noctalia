#pragma once

#include "core/files/resource_paths.h"
#include "ui/controls/image.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

// A theme-tinted SVG mask. The host sets its position and layering, then
// loads it with the dialog surface's renderer during layout.
class DialogBackgroundArtwork final : public Image {
public:
  DialogBackgroundArtwork(std::string_view filename, float width, float height, ColorRole role = ColorRole::Primary)
      : m_path(paths::assetPath("dialogue-window-artwork/" + std::string(filename)).string()) {
    setHitTestVisible(false);
    setParticipatesInLayout(false);
    setForegroundTint(colorSpecFromRole(role));
    setFrameSize(width, height);
  }

  bool load(Renderer& renderer) {
    const auto targetSize = static_cast<int>(std::ceil(std::max(width(), height())));
    return setSourceFile(renderer, m_path, targetSize);
  }

private:
  std::string m_path;
};
