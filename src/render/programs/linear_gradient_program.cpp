#include "render/programs/linear_gradient_program.h"

#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec4 u_rect;
varying vec2 v_uv;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 pixel_pos = u_rect.xy + (a_position * u_rect.zw);
    v_uv = a_position;
    gl_Position = vec4(to_ndc(pixel_pos), 0.0, 1.0);
}
)";

  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

uniform vec4 u_start_color;
uniform vec4 u_end_color;
uniform vec2 u_direction;
varying vec2 v_uv;

void main() {
    float t = clamp(dot(v_uv, u_direction), 0.0, 1.0);
    vec4 color = mix(u_start_color, u_end_color, t);
    gl_FragColor = vec4(color.rgb * color.a, color.a);
}
)";

} // namespace

void LinearGradientProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_rectLocation = glGetUniformLocation(m_program.id(), "u_rect");
  m_startColorLocation = glGetUniformLocation(m_program.id(), "u_start_color");
  m_endColorLocation = glGetUniformLocation(m_program.id(), "u_end_color");
  m_directionLocation = glGetUniformLocation(m_program.id(), "u_direction");

  if (m_positionLocation < 0 || m_surfaceSizeLocation < 0 || m_rectLocation < 0 || m_startColorLocation < 0 ||
      m_endColorLocation < 0 || m_directionLocation < 0) {
    throw std::runtime_error("failed to query linear-gradient shader locations");
  }
}

void LinearGradientProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_surfaceSizeLocation = -1;
  m_rectLocation = -1;
  m_startColorLocation = -1;
  m_endColorLocation = -1;
  m_directionLocation = -1;
}

void LinearGradientProgram::draw(
    float surfaceWidth, float surfaceHeight, float x, float y, float width, float height,
    const LinearGradientStyle& style
) const {
  if (!m_program.isValid() || width <= 0.0f || height <= 0.0f) {
    return;
  }

  const std::array<GLfloat, 12> vertices = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform4f(m_rectLocation, x, y, width, height);
  glUniform4f(m_startColorLocation, style.start.r, style.start.g, style.start.b, style.start.a);
  glUniform4f(m_endColorLocation, style.end.r, style.end.g, style.end.b, style.end.a);
  glUniform2f(m_directionLocation, style.horizontal ? 1.0f : 0.0f, style.horizontal ? 0.0f : 1.0f);
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
