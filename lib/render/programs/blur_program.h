#pragma once

#include "render/core/shader_program.h"

#include <GLES2/gl2.h>
#include <cstdint>

class TextureId;

class BlurProgram {
public:
  void ensureInitialized();
  void destroy();
  void abandon() noexcept;

  // Draw srcTex to the currently-bound framebuffer using a separable Gaussian blur.
  // dirX/dirY: blur direction (1,0 = horizontal, 0,1 = vertical).
  // radius: kernel half-width in texels (0 = no blur, 20 = maximum).
  void draw(TextureId srcTex, std::uint32_t width, std::uint32_t height, float dirX, float dirY, float radius) const;

private:
  ShaderProgram m_program;
  GLint m_posLoc = -1;
  GLint m_texLoc = -1;
  GLint m_texelSzLoc = -1;
  GLint m_directionLoc = -1;
  GLint m_radiusLoc = -1;
};
