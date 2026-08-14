#include "render/programs/wallpaper_mask_program.h"

#include "render/programs/wallpaper_sampling_glsl.h"

#include <array>
#include <stdexcept>
#include <string>

namespace {

  constexpr char kVertexShader[] = R"(
precision highp float;
attribute vec2 a_position;
uniform vec2 u_surfaceSize;
varying vec2 v_surfaceUV;

void main() {
    v_surfaceUV = a_position;
    vec2 pixel = a_position * u_surfaceSize;
    vec2 normalized = pixel / u_surfaceSize;
    gl_Position = vec4(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0, 0.0, 1.0);
}
)";

  constexpr char kFragmentPrefix[] = R"(
precision highp float;
uniform sampler2D u_mask;
uniform vec2 u_surfaceSize;
uniform vec2 u_surfaceOffset;
uniform vec2 u_outputSize;
uniform vec2 u_imageSize;
varying vec2 v_surfaceUV;
)";

  constexpr char kFragmentMain[] = R"(
void main() {
    vec2 outputPixel = u_surfaceOffset + v_surfaceUV * u_surfaceSize;
    vec2 outputUV = outputPixel / u_outputSize;
    vec2 maskUV = calculateWallpaperUV(outputUV, u_imageSize.x, u_imageSize.y);

    float coverage = 0.0;
    if (u_fillMode > 3.5 && u_fillMode < 4.5) {
        coverage = texture2D(u_mask, fract(maskUV)).a;
    } else if (!wallpaperUVOutside(maskUV)) {
        coverage = texture2D(u_mask, maskUV).a;
    }
    gl_FragColor = vec4(0.0, 0.0, 0.0, coverage);
}
)";

} // namespace

void WallpaperMaskProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  const std::string fragment =
      std::string(kFragmentPrefix) + std::string(wallpaper_shader::kSamplingSource) + kFragmentMain;
  m_program.create(kVertexShader, fragment.c_str());

  const GLuint id = m_program.id();
  m_positionLoc = glGetAttribLocation(id, "a_position");
  m_surfaceSizeLoc = glGetUniformLocation(id, "u_surfaceSize");
  m_maskLoc = glGetUniformLocation(id, "u_mask");
  m_surfaceOffsetLoc = glGetUniformLocation(id, "u_surfaceOffset");
  m_outputSizeLoc = glGetUniformLocation(id, "u_outputSize");
  m_fillModeLoc = glGetUniformLocation(id, "u_fillMode");
  m_screenWidthLoc = glGetUniformLocation(id, "u_screenWidth");
  m_screenHeightLoc = glGetUniformLocation(id, "u_screenHeight");
  m_imageSizeLoc = glGetUniformLocation(id, "u_imageSize");
  m_spanOffsetLoc = glGetUniformLocation(id, "u_spanOffset");
  m_spanMonitorSizeLoc = glGetUniformLocation(id, "u_spanMonitorSize");
  m_spanTotalSizeLoc = glGetUniformLocation(id, "u_spanTotalSize");

  if (m_positionLoc < 0
      || m_surfaceSizeLoc < 0
      || m_maskLoc < 0
      || m_surfaceOffsetLoc < 0
      || m_outputSizeLoc < 0
      || m_fillModeLoc < 0
      || m_screenWidthLoc < 0
      || m_screenHeightLoc < 0
      || m_imageSizeLoc < 0
      || m_spanOffsetLoc < 0
      || m_spanMonitorSizeLoc < 0
      || m_spanTotalSizeLoc < 0) {
    throw std::runtime_error("failed to query wallpaper mask shader locations");
  }
}

void WallpaperMaskProgram::destroy() { m_program.destroy(); }

void WallpaperMaskProgram::abandon() noexcept { m_program.abandon(); }

void WallpaperMaskProgram::draw(const WallpaperMaskDrawParams& params) const {
  if (!m_program.isValid()
      || params.texture == 0
      || params.surfaceWidth <= 0.0F
      || params.surfaceHeight <= 0.0F
      || params.outputWidth <= 0.0F
      || params.outputHeight <= 0.0F
      || params.imageWidth <= 0.0F
      || params.imageHeight <= 0.0F) {
    return;
  }

  constexpr std::array<GLfloat, 12> kQuad = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLoc, params.surfaceWidth, params.surfaceHeight);
  glUniform2f(m_surfaceOffsetLoc, params.surfaceOffsetX, params.surfaceOffsetY);
  glUniform2f(m_outputSizeLoc, params.outputWidth, params.outputHeight);
  glUniform1f(m_fillModeLoc, params.fillMode);
  glUniform1f(m_screenWidthLoc, params.outputWidth);
  glUniform1f(m_screenHeightLoc, params.outputHeight);
  glUniform2f(m_imageSizeLoc, params.imageWidth, params.imageHeight);
  glUniform2f(m_spanOffsetLoc, params.span.offsetX, params.span.offsetY);
  glUniform2f(m_spanMonitorSizeLoc, params.span.monitorWidth, params.span.monitorHeight);
  glUniform2f(m_spanTotalSizeLoc, params.span.totalWidth, params.span.totalHeight);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(params.texture.value()));
  glUniform1i(m_maskLoc, 0);

  glEnableVertexAttribArray(static_cast<GLuint>(m_positionLoc));
  glVertexAttribPointer(static_cast<GLuint>(m_positionLoc), 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(static_cast<GLuint>(m_positionLoc));
}
