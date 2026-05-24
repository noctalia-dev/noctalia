#pragma once

#include "render/core/mat3.h"
#include "render/core/render_styles.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

class SpinnerProgram {
public:
  SpinnerProgram() = default;
  ~SpinnerProgram() = default;

  SpinnerProgram(const SpinnerProgram&) = delete;
  SpinnerProgram& operator=(const SpinnerProgram&) = delete;

  void ensureInitialized();
  void destroy();

  void draw(
      float surfaceWidth, float surfaceHeight, float width, float height, const SpinnerStyle& style,
      const Mat3& transform = Mat3::identity()
  ) const;

private:
  ShaderProgram m_program;
  GLint m_positionLocation = -1;
  GLint m_surfaceSizeLocation = -1;
  GLint m_quadSizeLocation = -1;
  GLint m_rectOriginLocation = -1;
  GLint m_rectSizeLocation = -1;
  GLint m_colorLocation = -1;
  GLint m_thicknessLocation = -1;
  GLint m_transformLocation = -1;
};
