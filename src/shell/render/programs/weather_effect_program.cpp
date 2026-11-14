#include "shell/render/programs/weather_effect_program.h"

#include "render/core/texture_handle.h"
#include "shell/render/programs/weather_effect_cloud_glsl.h"
#include "shell/render/programs/weather_effect_common_glsl.h"
#include "shell/render/programs/weather_effect_rain_glsl.h"
#include "shell/render/programs/weather_effect_snow_glsl.h"
#include "shell/render/programs/weather_effect_stars_glsl.h"
#include "shell/render/programs/weather_effect_sun_glsl.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

  // Slot in m_programs. Fog has no slot of its own; draw() renders it with the Cloud program.
  std::optional<std::size_t> programIndex(WeatherEffectType type) {
    switch (type) {
    case WeatherEffectType::Sun:
      return 0;
    case WeatherEffectType::Snow:
      return 1;
    case WeatherEffectType::Rain:
      return 2;
    case WeatherEffectType::Cloud:
      return 3;
    case WeatherEffectType::Stars:
      return 4;
    case WeatherEffectType::None:
    case WeatherEffectType::Fog:
      return std::nullopt;
    }
    return std::nullopt;
  }

} // namespace

void WeatherEffectProgram::ensureInitialized() {
  if (m_programs[0].program.isValid()) {
    return;
  }

  initProgram(WeatherEffectType::Sun, weather_effect_shader::kSunFragment);
  initProgram(WeatherEffectType::Snow, weather_effect_shader::kSnowFragment);
  initProgram(WeatherEffectType::Rain, weather_effect_shader::kRainFragment);
  initProgram(WeatherEffectType::Cloud, weather_effect_shader::kCloudFragment);
  initProgram(WeatherEffectType::Stars, weather_effect_shader::kStarsFragment);
}

void WeatherEffectProgram::destroy() {
  for (auto& pd : m_programs) {
    pd.program.destroy();
  }
}

void WeatherEffectProgram::abandon() noexcept {
  for (auto& pd : m_programs) {
    pd.program.abandon();
  }
}

void WeatherEffectProgram::initProgram(WeatherEffectType type, const char* fragSource) {
  std::string fullFrag = std::string(weather_effect_shader::kCommonFragment) + fragSource;

  auto& pd = m_programs[programIndex(type).value()];
  pd.program.create(weather_effect_shader::kVertexShader, fullFrag.c_str());

  auto id = pd.program.id();
  pd.positionLoc = glGetAttribLocation(id, "a_position");
  pd.surfaceSizeLoc = glGetUniformLocation(id, "u_surface_size");
  pd.quadSizeLoc = glGetUniformLocation(id, "u_quad_size");
  pd.transformLoc = glGetUniformLocation(id, "u_transform");
  pd.timeLoc = glGetUniformLocation(id, "u_time");
  pd.itemWidthLoc = glGetUniformLocation(id, "u_item_width");
  pd.itemHeightLoc = glGetUniformLocation(id, "u_item_height");
  pd.bgColorLoc = glGetUniformLocation(id, "u_bg_color");
  pd.accentColorLoc = glGetUniformLocation(id, "u_accent_color");
  pd.radiusLoc = glGetUniformLocation(id, "u_radius");
  pd.alternativeLoc = glGetUniformLocation(id, "u_alternative");
  pd.particlesLoc = glGetUniformLocation(id, "u_particles");
  pd.particleCountLoc = glGetUniformLocation(id, "u_particle_count");

  if (pd.positionLoc < 0 || pd.surfaceSizeLoc < 0 || pd.transformLoc < 0) {
    throw std::runtime_error("failed to query effect shader locations");
  }
}

void WeatherEffectProgram::draw(
    TextureId particles, float surfaceWidth, float surfaceHeight, float width, float height,
    const WeatherEffectStyle& style, const Mat3& transform
) const {
  if (style.type == WeatherEffectType::None || width <= 0.0F || height <= 0.0F) {
    return;
  }

  // Fog reuses the Cloud shader with alternative=1
  const bool isFog = style.type == WeatherEffectType::Fog;
  const auto effectType = isFog ? WeatherEffectType::Cloud : style.type;
  const auto idx = programIndex(effectType);
  if (!idx.has_value() || !m_programs[*idx].program.isValid()) {
    return;
  }

  const auto& pd = m_programs[*idx];

  static constexpr float kQuad[] = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  glUseProgram(pd.program.id());

  glUniform2f(pd.surfaceSizeLoc, surfaceWidth, surfaceHeight);
  glUniform2f(pd.quadSizeLoc, width, height);
  glUniformMatrix3fv(pd.transformLoc, 1, GL_FALSE, transform.m.data());

  if (pd.timeLoc >= 0) {
    glUniform1f(pd.timeLoc, style.time);
  }
  if (pd.itemWidthLoc >= 0) {
    glUniform1f(pd.itemWidthLoc, width);
  }
  if (pd.itemHeightLoc >= 0) {
    glUniform1f(pd.itemHeightLoc, height);
  }
  if (pd.bgColorLoc >= 0) {
    glUniform4f(pd.bgColorLoc, style.bgColor.r, style.bgColor.g, style.bgColor.b, style.bgColor.a);
  }
  if (pd.accentColorLoc >= 0) {
    glUniform4f(pd.accentColorLoc, style.accentColor.r, style.accentColor.g, style.accentColor.b, style.accentColor.a);
  }
  if (pd.radiusLoc >= 0) {
    glUniform1f(pd.radiusLoc, style.radius);
  }
  if (pd.alternativeLoc >= 0) {
    glUniform1f(pd.alternativeLoc, isFog ? 1.0F : 0.0F);
  }
  if (pd.particlesLoc >= 0) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(particles.value()));
    glUniform1i(pd.particlesLoc, 0);
  }
  if (pd.particleCountLoc >= 0) {
    glUniform1i(pd.particleCountLoc, particles.valid() ? style.particleCount : 0);
  }

  auto posAttr = static_cast<GLuint>(pd.positionLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
