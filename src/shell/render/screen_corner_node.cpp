#include "shell/render/screen_corner_node.h"

#include "render/backend/render_backend.h"
#include "shell/render/programs/screen_corner_program.h"

void ScreenCornerNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  auto style = m_style;
  style.color.a *= context.opacity;
  auto& program = backend.extension<ScreenCornerProgram>();
  program.ensureInitialized();
  program.draw(context.surfaceWidth, context.surfaceHeight, context.width, context.height, style, context.transform);
}
