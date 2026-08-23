#include "render/programs/screen_corner_program.h"

#include "render/core/render_styles.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_size;
uniform mat3 u_transform;
varying vec2 v_local;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_local = local;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kFragmentShaderSource[] = R"(
#extension GL_OES_standard_derivatives : require
precision highp float;

uniform vec2 u_size;
uniform vec4 u_color;
uniform int u_corner;
uniform float u_exponent;
varying vec2 v_local;

vec2 corner_center() {
    if (u_corner == 1) {
        return vec2(0.0, u_size.y);
    }
    if (u_corner == 2) {
        return vec2(0.0, 0.0);
    }
    if (u_corner == 3) {
        return vec2(u_size.x, 0.0);
    }
    return u_size;
}

void main() {
    vec2 radius = max(u_size, vec2(1.0));
    vec2 normalized = abs(v_local - corner_center()) / radius;
    float shape = pow(normalized.x, u_exponent) + pow(normalized.y, u_exponent) - 1.0;
    // The superellipse implicit function is not a distance: its gradient scales with the
    // exponent and varies along the curve, so a fixed window antialiases over a fraction of
    // a pixel. Normalize by the screen-space gradient for a one-device-pixel transition, and
    // ramp linearly so partial pixels carry true box-filter coverage of the outside region.
    float pixel_width = max(length(vec2(dFdx(shape), dFdy(shape))), 1e-6);
    float coverage = clamp(0.5 + shape / pixel_width, 0.0, 1.0);
    float alpha = u_color.a * coverage;
    if (alpha <= 0.0) {
        discard;
    }
    gl_FragColor = vec4(u_color.rgb * alpha, alpha);
}
)";

} // namespace

void ScreenCornerProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_sizeLocation = glGetUniformLocation(m_program.id(), "u_size");
  m_colorLocation = glGetUniformLocation(m_program.id(), "u_color");
  m_cornerLocation = glGetUniformLocation(m_program.id(), "u_corner");
  m_exponentLocation = glGetUniformLocation(m_program.id(), "u_exponent");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");

  if (m_positionLocation < 0
      || m_surfaceSizeLocation < 0
      || m_sizeLocation < 0
      || m_colorLocation < 0
      || m_cornerLocation < 0
      || m_exponentLocation < 0
      || m_transformLocation < 0) {
    throw std::runtime_error("failed to query screen-corner shader locations");
  }
}

void ScreenCornerProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_surfaceSizeLocation = -1;
  m_sizeLocation = -1;
  m_colorLocation = -1;
  m_cornerLocation = -1;
  m_exponentLocation = -1;
  m_transformLocation = -1;
}

void ScreenCornerProgram::abandon() noexcept { m_program.abandon(); }

void ScreenCornerProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, const ScreenCornerStyle& style,
    const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0F || height <= 0.0F || style.color.a <= 0.0F) {
    return;
  }

  const std::array<GLfloat, 12> vertices = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_sizeLocation, width, height);
  glUniform4f(m_colorLocation, style.color.r, style.color.g, style.color.b, style.color.a);
  glUniform1i(m_cornerLocation, static_cast<GLint>(style.position));
  glUniform1f(m_exponentLocation, std::max(1.0F, style.exponent));
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, transform.m.data());
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
