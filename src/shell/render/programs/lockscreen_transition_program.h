#pragma once

#include "render/backend/render_extension.h"
#include "render/core/shader_program.h"
#include "shell/render/lockscreen_transition_types.h"

#include <GLES2/gl2.h>

class LockscreenTransitionProgram final : public RenderExtension {
public:
  void ensureInitialized();
  void destroy() override;
  void abandon() noexcept override;
  void draw(const LockscreenTransitionDrawParams& params) const;

private:
  ShaderProgram m_program;
  GLint m_positionLoc = -1;
  GLint m_surfaceSizeLoc = -1;
  GLint m_quadSizeLoc = -1;
  GLint m_transformLoc = -1;
  GLint m_textureLoc = -1;
  GLint m_transitionLoc = -1;
  GLint m_progressLoc = -1;
  GLint m_opacityLoc = -1;
  GLint m_directionLoc = -1;
  GLint m_smoothnessLoc = -1;
  GLint m_centerLoc = -1;
  GLint m_aspectRatioLoc = -1;
  GLint m_stripeCountLoc = -1;
  GLint m_angleLoc = -1;
  GLint m_cellSizeLoc = -1;
};
