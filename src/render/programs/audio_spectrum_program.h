#pragma once

#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>
#include <span>
#include <vector>

struct AudioSpectrumStyle;

class AudioSpectrumProgram {
public:
  AudioSpectrumProgram() = default;
  ~AudioSpectrumProgram() = default;

  AudioSpectrumProgram(const AudioSpectrumProgram&) = delete;
  AudioSpectrumProgram& operator=(const AudioSpectrumProgram&) = delete;

  void ensureInitialized();
  void destroy();
  void abandon() noexcept;

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
