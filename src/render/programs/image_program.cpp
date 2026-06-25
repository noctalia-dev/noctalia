#include "render/programs/image_program.h"

#include "render/core/texture_handle.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec2 a_texcoord;
uniform vec2 u_surface_size;
uniform vec2 u_size;
uniform vec2 u_tex_size;
uniform int u_fit_mode;
uniform mat3 u_transform;
varying vec2 v_texcoord;
varying vec2 v_local;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_size;
    vec3 pixel = u_transform * vec3(local, 1.0);

    vec2 scale = vec2(1.0);
    if (u_fit_mode != 0 && u_tex_size.x > 0.0 && u_tex_size.y > 0.0 && u_size.x > 0.0 && u_size.y > 0.0) {
        float rect_aspect = u_size.x / u_size.y;
        float tex_aspect = u_tex_size.x / u_tex_size.y;
        if (u_fit_mode == 1) { // Cover
            if (tex_aspect > rect_aspect) {
                scale.x = rect_aspect / tex_aspect;
            } else {
                scale.y = tex_aspect / rect_aspect;
            }
        } else { // Contain
            if (tex_aspect > rect_aspect) {
                scale.y = tex_aspect / rect_aspect;
            } else {
                scale.x = rect_aspect / tex_aspect;
            }
        }
    }
    vec2 offset = (vec2(1.0) - scale) * 0.5;
    v_texcoord = offset + a_texcoord * scale;
    v_local = local;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

uniform sampler2D u_texture;
uniform vec4 u_tint;
uniform int u_monochrome;
uniform int u_alpha_mask;
uniform float u_opacity;
uniform vec2 u_size;
uniform float u_radius;
uniform vec4 u_border_color;
uniform float u_border_width;
varying vec2 v_texcoord;
varying vec2 v_local;

float rounded_rect_distance(vec2 centered, vec2 half_size, float radius) {
    vec2 q = abs(centered) - (half_size - vec2(radius));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

void main() {
    float aa = 0.85;
    float radius = min(u_radius, min(u_size.x, u_size.y) * 0.5);
    vec2 centered = v_local - u_size * 0.5;
    float outer_distance = rounded_rect_distance(centered, u_size * 0.5, radius);
    float outer_coverage = 1.0 - smoothstep(-aa, aa, outer_distance);
    if (outer_coverage <= 0.0) {
        discard;
    }

    vec2 sample_uv = clamp(v_texcoord, vec2(0.0), vec2(1.0));
    vec4 texel = texture2D(u_texture, sample_uv);
    if (v_texcoord.x < 0.0 || v_texcoord.x > 1.0 || v_texcoord.y < 0.0 || v_texcoord.y > 1.0) {
        texel = vec4(0.0);
    }
    vec4 fill;
    if (u_monochrome != 0) {
        float coverage = u_alpha_mask != 0 ? texel.a : dot(texel.rgb, vec3(0.299, 0.587, 0.114)) * texel.a;
        fill = vec4(u_tint.rgb * coverage, coverage * u_tint.a);
    } else {
        fill = texel * u_tint;
    }
    fill *= vec4(1.0, 1.0, 1.0, u_opacity);

    if (u_border_width <= 0.0 || u_border_color.a <= 0.0) {
        float a = fill.a * outer_coverage;
        gl_FragColor = vec4(fill.rgb * a, a);
        return;
    }

    float inner_radius = max(radius - u_border_width, 0.0);
    vec2 inner_half = max(u_size * 0.5 - vec2(u_border_width), vec2(0.0));
    float inner_distance = rounded_rect_distance(centered, inner_half, inner_radius);
    float inner_coverage = 1.0 - smoothstep(-aa, aa, inner_distance);

    // Match the rect shader: the border is a stroke ring, not a full-area
    // backplane behind transparent image pixels.
    vec3 border_pm = u_border_color.rgb * u_border_color.a * u_opacity;
    float border_a = u_border_color.a * u_opacity;
    vec3 fill_pm = fill.rgb * fill.a;

    vec3 interior_rgb = mix(border_pm, fill_pm, inner_coverage);
    float interior_a = mix(border_a, fill.a, inner_coverage);

    float out_a = interior_a * outer_coverage;
    gl_FragColor = vec4(interior_rgb * outer_coverage, out_a);
}
)";

} // namespace

void ImageProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_texCoordLocation = glGetAttribLocation(m_program.id(), "a_texcoord");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_rectLocation = glGetUniformLocation(m_program.id(), "u_size");
  m_tintLocation = glGetUniformLocation(m_program.id(), "u_tint");
  m_monochromeLocation = glGetUniformLocation(m_program.id(), "u_monochrome");
  m_alphaMaskLocation = glGetUniformLocation(m_program.id(), "u_alpha_mask");
  m_opacityLocation = glGetUniformLocation(m_program.id(), "u_opacity");
  m_radiusLocation = glGetUniformLocation(m_program.id(), "u_radius");
  m_borderColorLocation = glGetUniformLocation(m_program.id(), "u_border_color");
  m_borderWidthLocation = glGetUniformLocation(m_program.id(), "u_border_width");
  m_texSizeLocation = glGetUniformLocation(m_program.id(), "u_tex_size");
  m_fitModeLocation = glGetUniformLocation(m_program.id(), "u_fit_mode");
  m_samplerLocation = glGetUniformLocation(m_program.id(), "u_texture");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");

  if (m_positionLocation < 0
      || m_texCoordLocation < 0
      || m_surfaceSizeLocation < 0
      || m_rectLocation < 0
      || m_tintLocation < 0
      || m_monochromeLocation < 0
      || m_alphaMaskLocation < 0
      || m_opacityLocation < 0
      || m_radiusLocation < 0
      || m_borderColorLocation < 0
      || m_borderWidthLocation < 0
      || m_texSizeLocation < 0
      || m_fitModeLocation < 0
      || m_samplerLocation < 0
      || m_transformLocation < 0) {
    throw std::runtime_error("failed to query image shader locations");
  }
}

void ImageProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_texCoordLocation = -1;
  m_surfaceSizeLocation = -1;
  m_rectLocation = -1;
  m_tintLocation = -1;
  m_monochromeLocation = -1;
  m_alphaMaskLocation = -1;
  m_opacityLocation = -1;
  m_radiusLocation = -1;
  m_borderColorLocation = -1;
  m_borderWidthLocation = -1;
  m_texSizeLocation = -1;
  m_fitModeLocation = -1;
  m_samplerLocation = -1;
  m_transformLocation = -1;
}

void ImageProgram::draw(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, const Color& tint,
    bool monochromeTint, bool alphaMaskTint, float opacity, float radius, const Color& borderColor, float borderWidth,
    int fitMode, float textureWidth, float textureHeight, const Mat3& transform
) const {
  if (!m_program.isValid() || texture == 0 || width <= 0.0f || height <= 0.0f) {
    return;
  }

  const std::array<GLfloat, 12> positions = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  const std::array<GLfloat, 12> texcoords = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_rectLocation, width, height);
  glUniform4f(m_tintLocation, tint.r, tint.g, tint.b, tint.a);
  glUniform1i(m_monochromeLocation, monochromeTint ? 1 : 0);
  glUniform1i(m_alphaMaskLocation, alphaMaskTint ? 1 : 0);
  glUniform1f(m_opacityLocation, opacity);
  glUniform1f(m_radiusLocation, std::max(0.0f, radius));
  glUniform4f(m_borderColorLocation, borderColor.r, borderColor.g, borderColor.b, borderColor.a);
  glUniform1f(m_borderWidthLocation, std::max(0.0f, borderWidth));
  glUniform2f(m_texSizeLocation, textureWidth, textureHeight);
  glUniform1i(m_fitModeLocation, fitMode);
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
