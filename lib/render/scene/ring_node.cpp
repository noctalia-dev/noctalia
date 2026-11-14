#include "render/scene/ring_node.h"

#include "render/backend/render_backend.h"
#include "render/programs/ring_program.h"

#include <algorithm>

void RingNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  auto style = m_style;
  style.fill.a *= context.opacity;
  style.track.a *= context.opacity;
  auto& program = backend.extension<RingProgram>();
  program.ensureInitialized();
  program.draw(
      context.surfaceWidth, context.surfaceHeight, context.width, context.height,
      std::min(context.pixelScaleX, context.pixelScaleY), style, context.transform
  );
}
