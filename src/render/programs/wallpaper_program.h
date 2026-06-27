#pragma once

#include "config/config_types.h"
#include "render/core/color.h"
#include "render/core/mat3.h"
#include "render/core/shader_program.h"
#include "render/core/texture_handle.h"
#include "render/core/wallpaper_types.h"

#include <GLES2/gl2.h>
#include <array>
#include <cstdint>

class WallpaperProgram {
public:
  WallpaperProgram() = default;
  ~WallpaperProgram() = default;

  WallpaperProgram(const WallpaperProgram&) = delete;
  WallpaperProgram& operator=(const WallpaperProgram&) = delete;

  void ensureInitialized();
  void destroy();

  void draw(const WallpaperDrawParams& params) const;

private:
  static constexpr std::size_t kTransitionCount = 6;

  struct ProgramData {
    ShaderProgram program;
    GLint positionLoc = -1;
    GLint surfaceSizeLoc = -1;
    GLint quadSizeLoc = -1;
    GLint transformLoc = -1;
    // Samplers
    GLint source1Loc = -1;
    GLint source2Loc = -1;
    GLint sourceKind1Loc = -1;
    GLint sourceKind2Loc = -1;
    GLint sourceColor1Loc = -1;
    GLint sourceColor2Loc = -1;
    // Common uniforms
    GLint progressLoc = -1;
    GLint fillModeLoc = -1;
    GLint imageWidth1Loc = -1;
    GLint imageHeight1Loc = -1;
    GLint imageWidth2Loc = -1;
    GLint imageHeight2Loc = -1;
    GLint screenWidthLoc = -1;
    GLint screenHeightLoc = -1;
    GLint fillColorLoc = -1;
    GLint spanOffsetLoc = -1;
    GLint spanMonitorSizeLoc = -1;
    GLint spanTotalSizeLoc = -1;
    // Per-transition uniforms
    GLint directionLoc = -1;
    GLint smoothnessLoc = -1;
    GLint centerXLoc = -1;
    GLint centerYLoc = -1;
    GLint aspectRatioLoc = -1;
    GLint stripeCountLoc = -1;
    GLint angleLoc = -1;
    GLint maxBlockSizeLoc = -1;
    GLint cellSizeLoc = -1;
  };

  void initProgram(std::size_t index, const char* fragSource);

  std::array<ProgramData, kTransitionCount> m_programs;
};
