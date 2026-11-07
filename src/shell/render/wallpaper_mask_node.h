#pragma once

#include "render/scene/custom_draw_node.h"
#include "shell/render/wallpaper_types.h"

#include <optional>

// Punches the mask's opaque texels out of everything drawn before it. Rendered as a surface
// overlay after the scene, so the mask covers the whole surface.
class WallpaperMaskNode final : public CustomDrawNode {
public:
  WallpaperMaskNode() {
    setHitTestVisible(false);
    setParticipatesInLayout(false);
  }

  // Returns whether the mask changed.
  bool setParams(const std::optional<WallpaperMaskDrawParams>& params) {
    if (m_params == params) {
      return false;
    }
    m_params = params;
    markPaintDirty();
    return true;
  }

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

private:
  std::optional<WallpaperMaskDrawParams> m_params;
};
