#pragma once

#include "render/core/mat3.h"
#include "render/core/render_styles.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

class ScreenCornerProgram {
public:
  ScreenCornerProgram() = default;
  ~ScreenCornerProgram() = default;

  ScreenCornerProgram(const ScreenCornerProgram&) = delete;
  ScreenCornerProgram& operator=(const ScreenCornerProgram&) = delete;

  void ensureInitialized();
  void destroy();

  void draw(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const ScreenCornerStyle& style, const Mat3& transform = Mat3::identity()
  ) const;

private:
  ShaderProgram m_program;
  GLint m_positionLocation = -1;
  GLint m_surfaceSizeLocation = -1;
  GLint m_sizeLocation = -1;
  GLint m_pixelScaleLocation = -1;
  GLint m_colorLocation = -1;
  GLint m_cornerLocation = -1;
  GLint m_exponentLocation = -1;
  GLint m_softnessLocation = -1;
  GLint m_transformLocation = -1;
};
