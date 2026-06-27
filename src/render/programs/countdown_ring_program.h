#pragma once

#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

struct CountdownRingStyle;

class CountdownRingProgram {
public:
  CountdownRingProgram() = default;
  ~CountdownRingProgram() = default;

  CountdownRingProgram(const CountdownRingProgram&) = delete;
  CountdownRingProgram& operator=(const CountdownRingProgram&) = delete;

  void ensureInitialized();
  void destroy();

  void draw(
      float surfaceWidth, float surfaceHeight, float width, float height, const CountdownRingStyle& style,
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
  GLint m_progressLocation = -1;
  GLint m_transformLocation = -1;
};
