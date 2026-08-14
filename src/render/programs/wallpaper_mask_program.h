#pragma once

#include "render/core/shader_program.h"
#include "render/core/wallpaper_types.h"

#include <GLES2/gl2.h>

class WallpaperMaskProgram {
public:
  WallpaperMaskProgram() = default;
  ~WallpaperMaskProgram() = default;

  WallpaperMaskProgram(const WallpaperMaskProgram&) = delete;
  WallpaperMaskProgram& operator=(const WallpaperMaskProgram&) = delete;

  void ensureInitialized();
  void destroy();
  void abandon() noexcept;
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
