#include "shell/render/wallpaper_mask_node.h"

#include "render/backend/render_backend.h"
#include "shell/render/programs/wallpaper_mask_program.h"

void WallpaperMaskNode::draw(RenderBackend& backend, const CustomDrawContext& /*context*/) const {
  if (!m_params.has_value() || m_params->texture == 0) {
    return;
  }
  auto& program = backend.extension<WallpaperMaskProgram>();
  program.ensureInitialized();
  backend.setBlendMode(RenderBlendMode::DestinationOut);
  program.draw(*m_params);
  backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
}
