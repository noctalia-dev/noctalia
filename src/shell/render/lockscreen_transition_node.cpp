#include "shell/render/lockscreen_transition_node.h"

#include "render/backend/render_backend.h"
#include "shell/render/programs/lockscreen_transition_program.h"

void LockscreenTransitionNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  if (m_texture == 0) {
    return;
  }
  auto& program = backend.extension<LockscreenTransitionProgram>();
  program.ensureInitialized();
  program.draw(
      LockscreenTransitionDrawParams{
          .transition = m_transition,
          .texture = m_texture,
          .surfaceWidth = context.surfaceWidth,
          .surfaceHeight = context.surfaceHeight,
          .quadWidth = context.width,
          .quadHeight = context.height,
          .progress = m_progress,
          .opacity = context.opacity,
          .params = m_params,
          .transform = context.transform,
      }
  );
}
