#include "shell/render/audio_spectrum_node.h"

#include "render/backend/render_backend.h"
#include "shell/render/programs/audio_spectrum_program.h"

void AudioSpectrumNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  auto style = m_style;
  style.color1.a *= context.opacity;
  style.color2.a *= context.opacity;
  auto& program = backend.extension<AudioSpectrumProgram>();
  program.ensureInitialized();
  program.draw(
      context.surfaceWidth, context.surfaceHeight, context.pixelScaleX, context.pixelScaleY, context.width,
      context.height, style, m_values, context.transform
  );
}
