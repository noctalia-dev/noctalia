#pragma once

#include "render/backend/render_extension.h"
#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>
#include <span>
#include <vector>

struct AudioSpectrumStyle;

class AudioSpectrumProgram final : public RenderExtension {
public:
  void ensureInitialized();
  void destroy() override;
  void abandon() noexcept override;

  void draw(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const AudioSpectrumStyle& style, std::span<const float> values, const Mat3& transform = Mat3::identity()
  ) const;

private:
  ShaderProgram m_program;
  GLint m_positionLocation = -1;
  GLint m_colorLocation = -1;
  GLint m_surfaceSizeLocation = -1;
  GLint m_pixelScaleLocation = -1;
  GLint m_snapToDeviceLocation = -1;
  GLint m_transformLocation = -1;
  mutable std::vector<GLfloat> m_vertices;
};
