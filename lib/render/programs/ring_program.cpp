#include "render/programs/ring_program.h"

#include "render/core/render_styles.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform vec2 u_rect_origin;
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

  // Distances are logical px. The fill is the exact distance to a round-capped arc, so the caps
  // and the arc body blend without an angular seam; progress 0 draws no fill and 1 draws the
  // closed circle without caps.
  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

uniform vec2 u_rect_size;
uniform vec4 u_fill;
uniform vec4 u_track;
uniform float u_thickness;
uniform float u_progress;
uniform float u_aa;
varying vec2 v_pixel;

const float TWO_PI = 6.28318530718;

void main() {
    vec2 p = v_pixel - u_rect_size * 0.5;
    float radius = min(u_rect_size.x, u_rect_size.y) * 0.5 - u_thickness * 0.5;
    float halfStroke = u_thickness * 0.5;
    float ringDist = abs(length(p) - radius);

    float trackCov = 1.0 - smoothstep(-u_aa, u_aa, ringDist - halfStroke);

    float fillCov = 0.0;
    if (u_progress >= 1.0) {
        fillCov = trackCov;
    } else if (u_progress > 0.0) {
        // Clockwise angle from 12 o'clock in y-down coordinates.
        float angle = atan(p.x, -p.y);
        if (angle < 0.0) {
            angle += TWO_PI;
        }
        float sweep = TWO_PI * u_progress;
        float arcDist = ringDist;
        if (angle > sweep) {
            vec2 startCap = vec2(0.0, -radius);
            vec2 endCap = vec2(sin(sweep), -cos(sweep)) * radius;
            arcDist = min(length(p - startCap), length(p - endCap));
        }
        fillCov = 1.0 - smoothstep(-u_aa, u_aa, arcDist - halfStroke);
    }

    float fillA = fillCov * u_fill.a;
    float trackA = trackCov * u_track.a * (1.0 - fillA);
    float alpha = fillA + trackA;
    if (alpha <= 0.0) {
        discard;
    }
    gl_FragColor = vec4(u_fill.rgb * fillA + u_track.rgb * trackA, alpha);
}
)";

} // namespace

void RingProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_quadSizeLocation = glGetUniformLocation(m_program.id(), "u_quad_size");
  m_rectOriginLocation = glGetUniformLocation(m_program.id(), "u_rect_origin");
  m_rectSizeLocation = glGetUniformLocation(m_program.id(), "u_rect_size");
  m_fillLocation = glGetUniformLocation(m_program.id(), "u_fill");
  m_trackLocation = glGetUniformLocation(m_program.id(), "u_track");
  m_thicknessLocation = glGetUniformLocation(m_program.id(), "u_thickness");
  m_progressLocation = glGetUniformLocation(m_program.id(), "u_progress");
  m_aaLocation = glGetUniformLocation(m_program.id(), "u_aa");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");

  if (m_positionLocation < 0
      || m_surfaceSizeLocation < 0
      || m_quadSizeLocation < 0
      || m_rectOriginLocation < 0
      || m_rectSizeLocation < 0
      || m_fillLocation < 0
      || m_trackLocation < 0
      || m_thicknessLocation < 0
      || m_progressLocation < 0
      || m_aaLocation < 0
      || m_transformLocation < 0) {
    throw std::runtime_error("failed to query ring shader locations");
  }
}

void RingProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_surfaceSizeLocation = -1;
  m_quadSizeLocation = -1;
  m_rectOriginLocation = -1;
  m_rectSizeLocation = -1;
  m_fillLocation = -1;
  m_trackLocation = -1;
  m_thicknessLocation = -1;
  m_progressLocation = -1;
  m_aaLocation = -1;
  m_transformLocation = -1;
}

void RingProgram::abandon() noexcept { m_program.abandon(); }

void RingProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, float pixelScale, const RingStyle& style,
    const Mat3& transform
) const {
  const float diameter = std::min(width, height);
  if (!m_program.isValid() || diameter <= 0.0F || style.thickness <= 0.0F) {
    return;
  }

  const std::array<GLfloat, 12> vertices = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  // The stroke sits inside the bounds; one px of padding keeps the outer anti-aliased edge.
  const float aa = 0.5F / std::max(pixelScale, 0.01F);
  const float padding = 1.0F;
  const float quadWidth = width + padding * 2.0F;
  const float quadHeight = height + padding * 2.0F;
  const Mat3 quadTransform = transform * Mat3::translation(-padding, -padding);
  const float thickness = std::min(style.thickness, diameter * 0.5F);

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_quadSizeLocation, quadWidth, quadHeight);
  glUniform2f(m_rectOriginLocation, padding, padding);
  glUniform2f(m_rectSizeLocation, width, height);
  glUniform4f(m_fillLocation, style.fill.r, style.fill.g, style.fill.b, style.fill.a);
  glUniform4f(m_trackLocation, style.track.r, style.track.g, style.track.b, style.track.a);
  glUniform1f(m_thicknessLocation, thickness);
  glUniform1f(m_progressLocation, std::clamp(style.progress, 0.0F, 1.0F));
  glUniform1f(m_aaLocation, aa);
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, quadTransform.m.data());
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
