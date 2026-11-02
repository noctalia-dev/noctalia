#pragma once

#include "render/backend/render_extension.h"
#include "render/core/shader_program.h"
#include "shell/render/wallpaper_types.h"

#include <GLES2/gl2.h>

class WallpaperMaskProgram final : public RenderExtension {
public:
  void ensureInitialized();
  void destroy() override;
  void abandon() noexcept override;
  void draw(const WallpaperMaskDrawParams& params) const;

private:
  ShaderProgram m_program;
  GLint m_positionLoc = -1;
  GLint m_surfaceSizeLoc = -1;
  GLint m_maskLoc = -1;
  GLint m_surfaceOffsetLoc = -1;
  GLint m_outputSizeLoc = -1;
  GLint m_fillModeLoc = -1;
  GLint m_screenWidthLoc = -1;
  GLint m_screenHeightLoc = -1;
  GLint m_imageSizeLoc = -1;
  GLint m_spanOffsetLoc = -1;
  GLint m_spanMonitorSizeLoc = -1;
  GLint m_spanTotalSizeLoc = -1;
};
