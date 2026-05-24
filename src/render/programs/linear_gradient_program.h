#pragma once

#include "render/core/color.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

struct LinearGradientStyle {
  Color start{};
  Color end{};
  bool horizontal = true;
};

class LinearGradientProgram {
public:
  LinearGradientProgram() = default;
  ~LinearGradientProgram() = default;

  LinearGradientProgram(const LinearGradientProgram&) = delete;
  LinearGradientProgram& operator=(const LinearGradientProgram&) = delete;

  void ensureInitialized();
  void destroy();

  void draw(
      float surfaceWidth, float surfaceHeight, float x, float y, float width, float height,
      const LinearGradientStyle& style
  ) const;

private:
  ShaderProgram m_program;
  GLint m_positionLocation = -1;
  GLint m_surfaceSizeLocation = -1;
  GLint m_rectLocation = -1;
  GLint m_startColorLocation = -1;
  GLint m_endColorLocation = -1;
  GLint m_directionLocation = -1;
};
