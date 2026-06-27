#include "render/programs/glyph_program.h"

#include "render/core/color.h"
#include "render/core/texture_handle.h"

#include <array>
#include <stdexcept>

namespace {

  // Positions a unit quad, applies a pixel-space transform, converts to NDC.
  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec2 a_texcoord;
uniform vec2 u_surface_size;
uniform vec2 u_size;
uniform mat3 u_transform;
varying vec2 v_texcoord;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_texcoord = a_texcoord;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  // Fragment shader: two modes selected by u_tint_mode.
  //   mode 0 (RGBA): texture stores a premultiplied RGBA glyph; scale by opacity.
  //   mode 1 (tint): texture stores an alpha coverage mask; the output is
  //                  premul(tint) * coverage * opacity.
  // Both paths output premultiplied to match the pipeline-wide
  // GL_ONE / GL_ONE_MINUS_SRC_ALPHA blend mode.
  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

uniform sampler2D u_texture;
uniform float u_opacity;
uniform vec4 u_tint;        // straight (non-premul) rgba
uniform float u_tint_mode;  // 0 = RGBA texture, 1 = alpha coverage + u_tint
varying vec2 v_texcoord;

void main() {
    vec4 c = texture2D(u_texture, v_texcoord);
    if (u_tint_mode > 0.5) {
        float coverage = c.a * u_tint.a * u_opacity;
        gl_FragColor = vec4(u_tint.rgb * coverage, coverage);
    } else {
        gl_FragColor = vec4(c.rgb * u_opacity, c.a * u_opacity);
    }
}
)";

} // namespace

void GlyphProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_texCoordLocation = glGetAttribLocation(m_program.id(), "a_texcoord");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_rectLocation = glGetUniformLocation(m_program.id(), "u_size");
  m_opacityLocation = glGetUniformLocation(m_program.id(), "u_opacity");
  m_samplerLocation = glGetUniformLocation(m_program.id(), "u_texture");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");
  m_tintLocation = glGetUniformLocation(m_program.id(), "u_tint");
  m_tintModeLocation = glGetUniformLocation(m_program.id(), "u_tint_mode");

  if (m_positionLocation < 0
      || m_texCoordLocation < 0
      || m_surfaceSizeLocation < 0
      || m_rectLocation < 0
      || m_opacityLocation < 0
      || m_samplerLocation < 0
      || m_transformLocation < 0
      || m_tintLocation < 0
      || m_tintModeLocation < 0) {
    throw std::runtime_error("failed to query color glyph shader locations");
  }
}

void GlyphProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_texCoordLocation = -1;
  m_surfaceSizeLocation = -1;
  m_rectLocation = -1;
  m_opacityLocation = -1;
  m_samplerLocation = -1;
  m_transformLocation = -1;
  m_tintLocation = -1;
  m_tintModeLocation = -1;
}

void GlyphProgram::bindCommon(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const Mat3& transform
) const {
  const std::array<GLfloat, 12> positions = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  const std::array<GLfloat, 12> texcoords = {
      u0, v0, u1, v0, u0, v1, u0, v1, u1, v0, u1, v1,
  };

  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_rectLocation, width, height);
  glUniform1f(m_opacityLocation, opacity);
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, transform.m.data());
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture.value()));
  glUniform1i(m_samplerLocation, 0);
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  const auto texAttr = static_cast<GLuint>(m_texCoordLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, positions.data());
  glVertexAttribPointer(texAttr, 2, GL_FLOAT, GL_FALSE, 0, texcoords.data());
  glEnableVertexAttribArray(posAttr);
  glEnableVertexAttribArray(texAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
  glDisableVertexAttribArray(texAttr);
}

void GlyphProgram::draw(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const Mat3& transform
) const {
  if (!m_program.isValid() || texture == 0 || width <= 0.0f || height <= 0.0f) {
    return;
  }
  glUseProgram(m_program.id());
  glUniform1f(m_tintModeLocation, 0.0f);
  glUniform4f(m_tintLocation, 1.0f, 1.0f, 1.0f, 1.0f);
  bindCommon(texture, surfaceWidth, surfaceHeight, width, height, u0, v0, u1, v1, opacity, transform);
}

void GlyphProgram::drawTinted(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const Color& tint, const Mat3& transform
) const {
  if (!m_program.isValid() || texture == 0 || width <= 0.0f || height <= 0.0f) {
    return;
  }
  glUseProgram(m_program.id());
  glUniform1f(m_tintModeLocation, 1.0f);
  glUniform4f(m_tintLocation, tint.r, tint.g, tint.b, tint.a);
  bindCommon(texture, surfaceWidth, surfaceHeight, width, height, u0, v0, u1, v1, opacity, transform);
}
