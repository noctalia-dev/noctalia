#include "render/programs/lockscreen_transition_program.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShader[] = R"(
precision highp float;
attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform mat3 u_transform;
varying vec2 v_texcoord;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    v_texcoord = a_position;
    vec3 pixel = u_transform * vec3(a_position * u_quad_size, 1.0);
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kFragmentShader[] = R"(
precision highp float;
uniform sampler2D u_texture;
uniform float u_transition;
uniform float u_progress;
uniform float u_opacity;
uniform float u_direction;
uniform float u_smoothness;
uniform vec2 u_center;
uniform float u_aspect_ratio;
uniform float u_stripe_count;
uniform float u_angle;
uniform float u_cell_size;
varying vec2 v_texcoord;

vec2 hex_round(float q, float r) {
    float x = q;
    float z = r;
    float y = -x - z;
    float rx = floor(x + 0.5);
    float ry = floor(y + 0.5);
    float rz = floor(z + 0.5);
    float dx = abs(rx - x);
    float dy = abs(ry - y);
    float dz = abs(rz - z);
    if (dx > dy && dx > dz) {
        rx = -ry - rz;
    } else if (dy > dz) {
        ry = -rx - rz;
    } else {
        rz = -rx - ry;
    }
    return vec2(rx, rz);
}

float max_distance_to_corners(vec2 center, float aspect) {
    float result = distance(center, vec2(0.0, 0.0));
    result = max(result, distance(center, vec2(aspect, 0.0)));
    result = max(result, distance(center, vec2(0.0, 1.0)));
    return max(result, distance(center, vec2(aspect, 1.0)));
}

float wipe_coverage(vec2 uv, float progress, float smoothness) {
    float extended = progress * (1.0 + 2.0 * smoothness) - smoothness;
    float factor;
    if (u_direction < 0.5) {
        float edge = 1.0 - extended;
        factor = smoothstep(edge - smoothness, edge + smoothness, uv.x);
    } else if (u_direction < 1.5) {
        float edge = extended;
        factor = smoothstep(edge - smoothness, edge + smoothness, uv.x);
        factor = 1.0 - factor;
    } else if (u_direction < 2.5) {
        float edge = 1.0 - extended;
        factor = smoothstep(edge - smoothness, edge + smoothness, uv.y);
    } else {
        float edge = extended;
        factor = smoothstep(edge - smoothness, edge + smoothness, uv.y);
        factor = 1.0 - factor;
    }
    return 1.0 - factor;
}

float disc_coverage(vec2 uv, float progress, float smoothness) {
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    vec2 center = vec2(u_center.x * u_aspect_ratio, u_center.y);
    float max_dist = max_distance_to_corners(center, u_aspect_ratio);
    float radius = progress * (max_dist + 2.0 * smoothness) - smoothness;
    return smoothstep(radius - smoothness, radius + smoothness, distance(aspect_uv, center));
}

float stripe_coverage(vec2 uv, float progress, float smoothness) {
    float rad = radians(u_angle);
    float cos_a = cos(rad);
    float sin_a = sin(rad);
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    float stripe_coord = aspect_uv.x * cos_a + aspect_uv.y * sin_a;
    float perp_coord = -aspect_uv.x * sin_a + aspect_uv.y * cos_a;
    float max_stripe_coord = u_aspect_ratio * abs(cos_a) + abs(sin_a);
    float stripe_width = max_stripe_coord / max(u_stripe_count, 1.0);
    float stripe_pos = stripe_coord / max(stripe_width, 0.0001);
    float odd = mod(floor(stripe_pos), 2.0);
    float max_perp = u_aspect_ratio * abs(sin_a) + abs(cos_a);
    float delay = abs(perp_coord / max(max_perp, 0.0001)) * 0.3;
    float local_progress = clamp((progress - delay) / max(1.0 - delay, 0.0001), 0.0, 1.0);
    float local_fraction = fract(stripe_pos);
    if (odd > 0.5) {
        float edge = mix(1.0 + smoothness, -smoothness, local_progress);
        return 1.0 - smoothstep(edge - smoothness, edge + smoothness, local_fraction);
    }
    float edge = mix(-smoothness, 1.0 + smoothness, local_progress);
    return smoothstep(edge - smoothness, edge + smoothness, local_fraction);
}

float honeycomb_coverage(vec2 uv, float progress, float smoothness) {
    float size = max(u_cell_size, 0.0001);
    vec2 aspect_uv = vec2(uv.x * u_aspect_ratio, uv.y);
    float q = (aspect_uv.x * (2.0 / 3.0)) / size;
    float r = ((-aspect_uv.x / 3.0) + (sqrt(3.0) / 3.0) * aspect_uv.y) / size;
    vec2 hex = hex_round(q, r);
    vec2 cell_center = vec2(
        hex.x * 1.5 * size,
        (hex.x * sqrt(3.0) / 2.0 + hex.y * sqrt(3.0)) * size
    );
    vec2 center = vec2(u_center.x * u_aspect_ratio, u_center.y);
    float max_dist = max_distance_to_corners(center, u_aspect_ratio);
    float radius = progress * (max_dist + 2.0 * smoothness) - smoothness;
    return smoothstep(radius - smoothness, radius + smoothness, distance(cell_center, center));
}

void main() {
    float progress = clamp(u_progress, 0.0, 1.0);
    vec2 uv = v_texcoord;
    float coverage = 1.0 - progress;
    float smoothness = mix(0.001, 0.5, u_smoothness * u_smoothness);

    if (progress <= 0.00001) {
        coverage = 1.0;
    } else if (progress >= 0.99999) {
        coverage = 0.0;
    } else if (u_transition < 0.5) {
        coverage = 1.0 - progress;
    } else if (u_transition < 1.5) {
        coverage = wipe_coverage(uv, progress, smoothness);
    } else if (u_transition < 2.5) {
        coverage = disc_coverage(uv, progress, smoothness);
    } else if (u_transition < 3.5) {
        coverage = stripe_coverage(uv, progress, mix(0.001, 0.3, u_smoothness * u_smoothness));
    } else if (u_transition < 4.5) {
        float scale = 1.0 + 0.15 * progress;
        uv = (uv - 0.5) / scale + 0.5;
        coverage = 1.0 - progress;
    } else {
        coverage = honeycomb_coverage(uv, progress, smoothness);
    }

    vec4 texel = texture2D(u_texture, uv);
    float alpha = texel.a * clamp(coverage * u_opacity, 0.0, 1.0);
    gl_FragColor = vec4(texel.rgb * alpha, alpha);
}
)";

} // namespace

void LockscreenTransitionProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }
  m_program.create(kVertexShader, kFragmentShader);
  const auto id = m_program.id();
  m_positionLoc = glGetAttribLocation(id, "a_position");
  m_surfaceSizeLoc = glGetUniformLocation(id, "u_surface_size");
  m_quadSizeLoc = glGetUniformLocation(id, "u_quad_size");
  m_transformLoc = glGetUniformLocation(id, "u_transform");
  m_textureLoc = glGetUniformLocation(id, "u_texture");
  m_transitionLoc = glGetUniformLocation(id, "u_transition");
  m_progressLoc = glGetUniformLocation(id, "u_progress");
  m_opacityLoc = glGetUniformLocation(id, "u_opacity");
  m_directionLoc = glGetUniformLocation(id, "u_direction");
  m_smoothnessLoc = glGetUniformLocation(id, "u_smoothness");
  m_centerLoc = glGetUniformLocation(id, "u_center");
  m_aspectRatioLoc = glGetUniformLocation(id, "u_aspect_ratio");
  m_stripeCountLoc = glGetUniformLocation(id, "u_stripe_count");
  m_angleLoc = glGetUniformLocation(id, "u_angle");
  m_cellSizeLoc = glGetUniformLocation(id, "u_cell_size");
  if (m_positionLoc < 0
      || m_surfaceSizeLoc < 0
      || m_quadSizeLoc < 0
      || m_transformLoc < 0
      || m_textureLoc < 0
      || m_transitionLoc < 0
      || m_progressLoc < 0
      || m_opacityLoc < 0
      || m_directionLoc < 0
      || m_smoothnessLoc < 0
      || m_centerLoc < 0
      || m_aspectRatioLoc < 0
      || m_stripeCountLoc < 0
      || m_angleLoc < 0
      || m_cellSizeLoc < 0) {
    throw std::runtime_error("failed to query lockscreen transition shader locations");
  }
}

void LockscreenTransitionProgram::destroy() { m_program.destroy(); }

void LockscreenTransitionProgram::abandon() noexcept { m_program.abandon(); }

void LockscreenTransitionProgram::draw(const LockscreenTransitionDrawParams& params) const {
  if (!m_program.isValid()
      || params.texture == 0
      || params.surfaceWidth <= 0.0F
      || params.surfaceHeight <= 0.0F
      || params.quadWidth <= 0.0F
      || params.quadHeight <= 0.0F) {
    return;
  }

  static constexpr std::array<GLfloat, 12> kQuad = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLoc, params.surfaceWidth, params.surfaceHeight);
  glUniform2f(m_quadSizeLoc, params.quadWidth, params.quadHeight);
  glUniformMatrix3fv(m_transformLoc, 1, GL_FALSE, params.transform.m.data());
  glUniform1f(m_transitionLoc, static_cast<float>(params.transition));
  glUniform1f(m_progressLoc, std::clamp(params.progress, 0.0F, 1.0F));
  glUniform1f(m_opacityLoc, std::clamp(params.opacity, 0.0F, 1.0F));
  glUniform1f(m_directionLoc, params.params.direction);
  glUniform1f(m_smoothnessLoc, params.params.smoothness);
  glUniform2f(m_centerLoc, params.params.centerX, params.params.centerY);
  glUniform1f(m_aspectRatioLoc, params.params.aspectRatio);
  glUniform1f(m_stripeCountLoc, params.params.stripeCount);
  glUniform1f(m_angleLoc, params.params.angle);
  glUniform1f(m_cellSizeLoc, params.params.cellSize);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(params.texture.value()));
  glUniform1i(m_textureLoc, 0);

  const auto position = static_cast<GLuint>(m_positionLoc);
  glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glEnableVertexAttribArray(position);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(position);
}
