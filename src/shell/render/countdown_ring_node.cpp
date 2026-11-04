#include "shell/render/countdown_ring_node.h"

#include "render/backend/render_backend.h"
#include "shell/render/programs/countdown_ring_program.h"

void CountdownRingNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  auto style = m_style;
  style.color.a *= context.opacity;
  auto& program = backend.extension<CountdownRingProgram>();
  program.ensureInitialized();
  program.draw(context.surfaceWidth, context.surfaceHeight, context.width, context.height, style, context.transform);
}
