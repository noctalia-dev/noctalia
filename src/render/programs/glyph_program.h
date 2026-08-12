#pragma once

#include "render/core/mat3.h"
#include "render/core/render_styles.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

class TextureId;
struct Color;

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
  void abandon() noexcept;

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

  // Gradient path: the texture's alpha is glyph coverage, and the fill comes from
  // four stops evaluated across the whole laid-out text block rather than this
  // quad — `fullWidth`/`fullHeight`/`tileOffsetY` locate the quad inside that
  // block so tiled and multi-line text share one ramp.
  //
  // A non-zero glowRadius grows the drawn quad on all four sides. That expansion
  // happens here, in C++: the quad size grows, the transform shifts back by the
  // radius, and the texcoords are remapped so the glyph keeps its original
  // footprint inside the larger quad. Getting the remap wrong stretches the
  // glyph, which still looks like text — hence the explicit note.
  void drawGradient(
      TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0,
      float u1, float v1, float opacity, const TextGradientStyle& gradient, float fullWidth, float fullHeight,
      float tileOffsetY, float contentScale, const Mat3& transform = Mat3::identity()
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
  GLint m_gradientDirectionLocation = -1;
  GLint m_gradientOffsetLocation = -1;
  GLint m_gradientStopsLocation = -1;
  GLint m_gradientColor0Location = -1;
  GLint m_gradientColor1Location = -1;
  GLint m_gradientColor2Location = -1;
  GLint m_gradientColor3Location = -1;
  GLint m_gradientSpanLocation = -1;
  GLint m_gradientQuadOriginLocation = -1;
  GLint m_glowRadiusLocation = -1;
  GLint m_texelSizeLocation = -1;
};
