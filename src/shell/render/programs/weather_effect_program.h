#pragma once

#include "render/backend/render_extension.h"
#include "render/core/mat3.h"
#include "render/core/shader_program.h"
#include "shell/render/shell_render_styles.h"

#include <GLES2/gl2.h>
#include <array>
#include <cstdint>

class TextureId;

class WeatherEffectProgram final : public RenderExtension {
public:
  void ensureInitialized();
  void destroy() override;
  void abandon() noexcept override;

  // `particles` is WeatherEffectNode's particle texture; only the Rain and Snow programs sample it.
  void draw(
      TextureId particles, float surfaceWidth, float surfaceHeight, float width, float height,
      const WeatherEffectStyle& style, const Mat3& transform = Mat3::identity()
  ) const;

private:
  static constexpr std::size_t kEffectCount = 5;

  struct ProgramData {
    ShaderProgram program;
    GLint positionLoc = -1;
    GLint surfaceSizeLoc = -1;
    GLint quadSizeLoc = -1;
    GLint transformLoc = -1;
    GLint timeLoc = -1;
    GLint itemWidthLoc = -1;
    GLint itemHeightLoc = -1;
    GLint bgColorLoc = -1;
    // Fog and Cloud only: theme accent tint (WeatherEffectStyle::accentColor).
    GLint accentColorLoc = -1;
    GLint radiusLoc = -1;
    GLint alternativeLoc = -1;
    // Rain and Snow only: particle texture and live particle count.
    GLint particlesLoc = -1;
    GLint particleCountLoc = -1;
  };

  void initProgram(WeatherEffectType type, const char* fragSource);

  std::array<ProgramData, kEffectCount> m_programs;
};
