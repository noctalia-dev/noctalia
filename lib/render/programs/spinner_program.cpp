#include "render/programs/spinner_program.h"

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
varying vec2 v_pixel;

const float PI = 3.14159265359;
const float TWO_PI = 2.0 * PI;

void main() {
    vec2 center = u_rect_size * 0.5;
    float strokeWidth = max(u_thickness, 0.5);
    float radius = max(min(u_rect_size.x, u_rect_size.y) * 0.5 - strokeWidth * 0.5, strokeWidth * 0.5);
    vec2 p = v_pixel - center;
    float dist = length(p);
    float aa = max(0.75, min(1.25, strokeWidth * 0.35));

    float ringSdf = abs(dist - radius) - strokeWidth * 0.5;
    float ringMask = 1.0 - smoothstep(-aa, aa, ringSdf);

    // A faint track anchors the motion while the comet trail brightens toward its head.
    float theta = atan(p.y, p.x);
    float travel = mod(-theta + TWO_PI, TWO_PI);
    float trailLength = PI * 1.48;
    float angularAa = aa / max(radius, 1.0);
    float trailMask = 1.0 - smoothstep(trailLength - angularAa, trailLength + angularAa, travel);
    float trailProgress = clamp(1.0 - travel / trailLength, 0.0, 1.0);
    float trailOpacity = mix(0.08, 0.88, pow(trailProgress, 0.72));
    float coverage = ringMask * (0.12 + 0.88 * trailMask * trailOpacity);

    // The circular cap and restrained halo make the leading edge read as a moving point
    // instead of the square-cut end of a rotating ring.
    vec2 head = vec2(radius, 0.0);
    float headDistance = length(p - head);
    float headMask = 1.0 - smoothstep(strokeWidth * 0.5 - aa, strokeWidth * 0.5 + aa, headDistance);
    float halo = (1.0 - smoothstep(strokeWidth * 0.45, strokeWidth * 2.1 + aa, headDistance)) * 0.24;
    coverage = 1.0 - (1.0 - coverage) * (1.0 - headMask);
    coverage = 1.0 - (1.0 - coverage) * (1.0 - halo);

    float alpha = clamp(coverage, 0.0, 1.0) * u_color.a;
    if (alpha <= 0.0) {
        discard;
    }

    vec3 litColor = mix(u_color.rgb, vec3(1.0), headMask * 0.14);
    gl_FragColor = vec4(litColor * alpha, alpha);
}
)";

} // namespace

void SpinnerProgram::ensureInitialized() {
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
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");

  if (m_positionLocation < 0
      || m_surfaceSizeLocation < 0
      || m_quadSizeLocation < 0
      || m_rectOriginLocation < 0
      || m_rectSizeLocation < 0
      || m_colorLocation < 0
      || m_thicknessLocation < 0
      || m_transformLocation < 0) {
    throw std::runtime_error("failed to query spinner shader locations");
  }
}

void SpinnerProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_surfaceSizeLocation = -1;
  m_quadSizeLocation = -1;
  m_rectOriginLocation = -1;
  m_rectSizeLocation = -1;
  m_colorLocation = -1;
  m_thicknessLocation = -1;
  m_transformLocation = -1;
}

void SpinnerProgram::abandon() noexcept { m_program.abandon(); }

void SpinnerProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, const SpinnerStyle& style, const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0F || height <= 0.0F) {
    return;
  }

  const std::array<GLfloat, 12> vertices = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  const float padding = style.thickness * 2.5F + 2.0F;
  const float quadWidth = width + padding * 2.0F;
  const float quadHeight = height + padding * 2.0F;
  const Mat3 quadTransform = transform * Mat3::translation(-padding, -padding);

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_quadSizeLocation, quadWidth, quadHeight);
  glUniform2f(m_rectOriginLocation, padding, padding);
  glUniform2f(m_rectSizeLocation, width, height);
  glUniform4f(m_colorLocation, style.color.r, style.color.g, style.color.b, style.color.a);
  glUniform1f(m_thicknessLocation, style.thickness);
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, quadTransform.m.data());
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
