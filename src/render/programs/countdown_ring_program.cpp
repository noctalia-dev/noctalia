#include "render/programs/countdown_ring_program.h"

#include "render/core/render_styles.h"

#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform vec2 u_rect_origin;
uniform vec2 u_rect_size;
uniform mat3 u_transform;
varying vec2 v_pixel;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_quad_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_pixel = local - u_rect_origin;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

uniform vec2 u_rect_size;
uniform vec4 u_color;
uniform float u_thickness;
uniform float u_progress;
varying vec2 v_pixel;

const float PI = 3.14159265359;

void main() {
    vec2 center = u_rect_size * 0.5;
    float radius = min(u_rect_size.x, u_rect_size.y) * 0.5 - u_thickness * 0.5;
    vec2 p = v_pixel - center;
    float dist = length(p);

    float ring = abs(dist - radius) - u_thickness * 0.5;
    float aa = max(1.0, u_thickness * 0.18);
    float ringMask = 1.0 - smoothstep(-aa, aa, ring);

    float theta = atan(p.y, p.x);
    float start = -PI * 0.5;
    float rel = mod(theta - start + 2.0 * PI, 2.0 * PI);
    float arcLen = 2.0 * PI * clamp(u_progress, 0.0, 1.0);
    float arcMask = 1.0 - smoothstep(arcLen - 0.06, arcLen + 0.06, rel);

    float alpha = ringMask * arcMask * u_color.a;
    if (alpha <= 0.0) {
        discard;
    }

    gl_FragColor = vec4(u_color.rgb * alpha, alpha);
}
)";

} // namespace

void CountdownRingProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_quadSizeLocation = glGetUniformLocation(m_program.id(), "u_quad_size");
  m_rectOriginLocation = glGetUniformLocation(m_program.id(), "u_rect_origin");
  m_rectSizeLocation = glGetUniformLocation(m_program.id(), "u_rect_size");
  m_colorLocation = glGetUniformLocation(m_program.id(), "u_color");
  m_thicknessLocation = glGetUniformLocation(m_program.id(), "u_thickness");
  m_progressLocation = glGetUniformLocation(m_program.id(), "u_progress");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");

  if (m_positionLocation < 0
      || m_surfaceSizeLocation < 0
      || m_quadSizeLocation < 0
      || m_rectOriginLocation < 0
      || m_rectSizeLocation < 0
      || m_colorLocation < 0
      || m_thicknessLocation < 0
      || m_progressLocation < 0
      || m_transformLocation < 0) {
    throw std::runtime_error("failed to query countdown ring shader locations");
  }
}

void CountdownRingProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_surfaceSizeLocation = -1;
  m_quadSizeLocation = -1;
  m_rectOriginLocation = -1;
  m_rectSizeLocation = -1;
  m_colorLocation = -1;
  m_thicknessLocation = -1;
  m_progressLocation = -1;
  m_transformLocation = -1;
}

void CountdownRingProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, const CountdownRingStyle& style,
    const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0f || height <= 0.0f) {
    return;
  }

  const std::array<GLfloat, 12> vertices = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  const float padding = style.thickness + 2.0f;
  const float quadWidth = width + padding * 2.0f;
  const float quadHeight = height + padding * 2.0f;
  const Mat3 quadTransform = transform * Mat3::translation(-padding, -padding);

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_quadSizeLocation, quadWidth, quadHeight);
  glUniform2f(m_rectOriginLocation, padding, padding);
  glUniform2f(m_rectSizeLocation, width, height);
  glUniform4f(m_colorLocation, style.color.r, style.color.g, style.color.b, style.color.a);
  glUniform1f(m_thicknessLocation, style.thickness);
  glUniform1f(m_progressLocation, style.progress);
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, quadTransform.m.data());
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
