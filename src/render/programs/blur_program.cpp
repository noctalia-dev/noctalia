#include "render/programs/blur_program.h"

#include "render/core/texture_handle.h"

#include <stdexcept>

namespace {

  constexpr char kVertexShader[] = R"(
precision highp float;
attribute vec2 a_position;
varying vec2 v_texcoord;

void main() {
    v_texcoord = a_position;
    vec2 ndc = a_position * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

  // Separable Gaussian blur with a fixed 81-tap loop (-40..40).
  // u_radius controls how many taps are active; taps beyond u_radius are skipped.
  constexpr char kFragmentShader[] = R"(
precision highp float;
uniform sampler2D u_texture;
uniform vec2 u_texelSize;
uniform vec2 u_direction;
uniform float u_radius;
varying vec2 v_texcoord;

void main() {
    vec4 color = vec4(0.0);
    float total = 0.0;
    float sigma = max(u_radius / 2.0, 0.0001);
    for (int i = -40; i <= 40; i++) {
        if (abs(float(i)) > u_radius) continue;
        float fi = float(i);
        float w = exp(-fi * fi / (2.0 * sigma * sigma));
        vec2 offset = u_direction * u_texelSize * fi;
        color += texture2D(u_texture, v_texcoord + offset) * w;
        total += w;
    }
    gl_FragColor = color / total;
}
)";

} // namespace

void BlurProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShader, kFragmentShader);

  const auto id = m_program.id();
  m_posLoc = glGetAttribLocation(id, "a_position");
  m_texLoc = glGetUniformLocation(id, "u_texture");
  m_texelSzLoc = glGetUniformLocation(id, "u_texelSize");
  m_directionLoc = glGetUniformLocation(id, "u_direction");
  m_radiusLoc = glGetUniformLocation(id, "u_radius");

  if (m_posLoc < 0 || m_texLoc < 0 || m_texelSzLoc < 0 || m_directionLoc < 0 || m_radiusLoc < 0) {
    throw std::runtime_error("failed to query blur shader locations");
  }
}

void BlurProgram::destroy() { m_program.destroy(); }

void BlurProgram::abandon() noexcept { m_program.abandon(); }

void BlurProgram::draw(
    TextureId srcTex, std::uint32_t width, std::uint32_t height, float dirX, float dirY, float radius
) const {
  if (!m_program.isValid() || srcTex == 0) {
    return;
  }

  static constexpr float kQuad[] = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  glUseProgram(m_program.id());

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(srcTex.value()));
  glUniform1i(m_texLoc, 0);

  glUniform2f(m_texelSzLoc, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
  glUniform2f(m_directionLoc, dirX, dirY);
  glUniform1f(m_radiusLoc, radius);

  auto posAttr = static_cast<GLuint>(m_posLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);

  glActiveTexture(GL_TEXTURE0);
}
