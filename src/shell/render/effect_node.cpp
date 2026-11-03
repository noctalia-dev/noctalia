#include "shell/render/effect_node.h"

#include "render/backend/render_backend.h"
#include "shell/render/programs/effect_program.h"

void EffectNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  auto style = m_style;
  style.bgColor.a *= context.opacity;
  auto& program = backend.extension<EffectProgram>();
  program.ensureInitialized();
  program.draw(context.surfaceWidth, context.surfaceHeight, context.width, context.height, style, context.transform);
}
