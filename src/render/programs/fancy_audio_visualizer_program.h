#pragma once

#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

class TextureId;
struct FancyAudioVisualizerStyle;

class FancyAudioVisualizerProgram {
public:
  FancyAudioVisualizerProgram() = default;
  ~FancyAudioVisualizerProgram() = default;

  FancyAudioVisualizerProgram(const FancyAudioVisualizerProgram&) = delete;
  FancyAudioVisualizerProgram& operator=(const FancyAudioVisualizerProgram&) = delete;

  void ensureInitialized();
  void destroy();
  void abandon() noexcept;

  void draw(
      TextureId audioTexture, float surfaceWidth, float surfaceHeight, float width, float height,
      const FancyAudioVisualizerStyle& style, const Mat3& transform = Mat3::identity()
  ) const;

private:
  ShaderProgram m_program;
  GLint m_positionLoc = -1;
  GLint m_surfaceSizeLoc = -1;
  GLint m_quadSizeLoc = -1;
  GLint m_transformLoc = -1;
  GLint m_audioSourceLoc = -1;
  GLint m_timeLoc = -1;
  GLint m_itemWidthLoc = -1;
  GLint m_itemHeightLoc = -1;
  GLint m_primaryColorLoc = -1;
  GLint m_secondaryColorLoc = -1;
  GLint m_sensitivityLoc = -1;
  GLint m_rotationSpeedLoc = -1;
  GLint m_barWidthLoc = -1;
  GLint m_ringOpacityLoc = -1;
  GLint m_cornerRadiusLoc = -1;
  GLint m_bloomIntensityLoc = -1;
  GLint m_modeLoc = -1;
  GLint m_waveThicknessLoc = -1;
  GLint m_innerDiameterLoc = -1;
};
