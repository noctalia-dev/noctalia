#pragma once

#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

struct RoundedRectStyle;

class RectProgram {
public:
  RectProgram() = default;
  ~RectProgram() = default;

  RectProgram(const RectProgram&) = delete;
  RectProgram& operator=(const RectProgram&) = delete;

  void ensureInitialized();
  void destroy();

  void draw(
      float surfaceWidth, float surfaceHeight, float width, float height, const RoundedRectStyle& style,
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
  GLint m_borderColorLocation = -1;
  GLint m_fillModeLocation = -1;
  GLint m_gradientDirectionLocation = -1;
  GLint m_gradientStopsLocation = -1;
  GLint m_gradientColor0Location = -1;
  GLint m_gradientColor1Location = -1;
  GLint m_gradientColor2Location = -1;
  GLint m_gradientColor3Location = -1;
  GLint m_cornerShapesLocation = -1;
  GLint m_logicalInsetLocation = -1;
  GLint m_radiiLocation = -1;
  GLint m_softnessLocation = -1;
  GLint m_noAaLocation = -1;
  GLint m_invertFillLocation = -1;
  GLint m_borderWidthLocation = -1;
  GLint m_outerShadowLocation = -1;
  GLint m_shadowCutoutOffsetLocation = -1;
  GLint m_shadowExclusionLocation = -1;
  GLint m_shadowExclusionOffsetLocation = -1;
  GLint m_shadowExclusionSizeLocation = -1;
  GLint m_shadowExclusionCornerShapesLocation = -1;
  GLint m_shadowExclusionLogicalInsetLocation = -1;
  GLint m_shadowExclusionRadiiLocation = -1;
  GLint m_transformLocation = -1;
};
