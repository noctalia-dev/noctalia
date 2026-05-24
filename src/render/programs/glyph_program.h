#pragma once

#include "render/core/color.h"
#include "render/core/mat3.h"
#include "render/core/shader_program.h"
#include "render/core/texture_handle.h"

#include <GLES2/gl2.h>

// Renders a glyph quad from either:
//   - a pre-rasterized premultiplied-RGBA texture (colored emoji, etc.), or
//   - an alpha-only (GL_ALPHA) coverage texture that gets tinted by u_tint.
// Used by the Pango/Cairo text renderer and the FreeType/Cairo icon renderer.
class GlyphProgram {
public:
  GlyphProgram() = default;
  ~GlyphProgram() = default;

  GlyphProgram(const GlyphProgram&) = delete;
  GlyphProgram& operator=(const GlyphProgram&) = delete;

  void ensureInitialized();
  void destroy();

  // RGBA path: sample the texture as premultiplied RGBA, scale by opacity.
  void draw(
      TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0,
      float u1, float v1, float opacity, const Mat3& transform = Mat3::identity()
  ) const;

  // Alpha-tint path: sample the texture's alpha channel as coverage, multiply
  // by `tint` (which is interpreted as straight RGBA — the shader premultiplies
  // it internally), scale by opacity.
  void drawTinted(
      TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0,
      float u1, float v1, float opacity, const Color& tint, const Mat3& transform = Mat3::identity()
  ) const;

private:
  void bindCommon(
      TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0,
      float u1, float v1, float opacity, const Mat3& transform
  ) const;

  ShaderProgram m_program;
  GLint m_positionLocation = -1;
  GLint m_texCoordLocation = -1;
  GLint m_surfaceSizeLocation = -1;
  GLint m_rectLocation = -1;
  GLint m_opacityLocation = -1;
  GLint m_samplerLocation = -1;
  GLint m_transformLocation = -1;
  GLint m_tintLocation = -1;
  GLint m_tintModeLocation = -1;
};
