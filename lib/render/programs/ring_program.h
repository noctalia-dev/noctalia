#pragma once

#include "render/backend/render_extension.h"
#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

struct RingStyle;

// Draws RingStyle: a track circle plus a round-capped clockwise arc, centred in the given bounds.
class RingProgram final : public RenderExtension {
public:
  void ensureInitialized();
  void destroy() override;
  void abandon() noexcept override;

  // pixelScale is buffer pixels per logical pixel; it sets the anti-aliasing width.
  void draw(
      float surfaceWidth, float surfaceHeight, float width, float height, float pixelScale, const RingStyle& style,
      const Mat3& transform = Mat3::identity()
  ) const;

private:
  ShaderProgram m_program;
  GLint m_positionLocation = -1;
  GLint m_surfaceSizeLocation = -1;
  GLint m_quadSizeLocation = -1;
  GLint m_rectOriginLocation = -1;
  GLint m_rectSizeLocation = -1;
  GLint m_fillLocation = -1;
  GLint m_trackLocation = -1;
  GLint m_thicknessLocation = -1;
  GLint m_progressLocation = -1;
  GLint m_aaLocation = -1;
  GLint m_transformLocation = -1;
};
