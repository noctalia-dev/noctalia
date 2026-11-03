#include "shell/render/wallpaper_node.h"

#include "shell/render/programs/wallpaper_program.h"

void WallpaperNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  const bool hasSource1 = m_sourceKind1 == WallpaperSourceKind::Color || m_texture1 != 0;
  if (!hasSource1) {
    return;
  }
  const bool hasSource2 = m_sourceKind2 == WallpaperSourceKind::Color || m_texture2 != 0;
  drawWallpaper(
      backend,
      WallpaperDrawParams{
          .transition = m_transition,
          .from =
              {.kind = m_sourceKind1,
               .texture = m_texture1,
               .color = m_sourceColor1,
               .imageWidth = m_imageWidth1,
               .imageHeight = m_imageHeight1},
          .to =
              {.kind = hasSource2 ? m_sourceKind2 : m_sourceKind1,
               .texture = hasSource2 ? m_texture2 : m_texture1,
               .color = hasSource2 ? m_sourceColor2 : m_sourceColor1,
               .imageWidth = hasSource2 ? m_imageWidth2 : m_imageWidth1,
               .imageHeight = hasSource2 ? m_imageHeight2 : m_imageHeight1},
          .surfaceWidth = context.surfaceWidth,
          .surfaceHeight = context.surfaceHeight,
          .quadWidth = context.width,
          .quadHeight = context.height,
          .progress = hasSource2 ? m_progress : 0.0F,
          .fillMode = static_cast<float>(m_fillMode),
          .params = m_params,
          .fillColor = m_fillColor,
          .transform = context.transform,
          .span = m_span,
      }
  );
}
