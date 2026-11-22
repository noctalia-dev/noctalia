#include "fancy_audio_visualizer_widget.h"

#include "ui/style.h"

#include <cmath>
#include <memory>

DesktopFancyAudioVisualizer::DesktopFancyAudioVisualizer() {
  setClipChildren(true);
  auto visualizer = std::make_unique<FancyAudioVisualizer>();
  m_visualizer = visualizer.get();
  addChild(std::move(visualizer));
}

void DesktopFancyAudioVisualizer::setContentScale(float scale) {
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

void DesktopFancyAudioVisualizer::doLayout(Renderer& renderer) {
  (void)renderer;
  const float size = std::round(300.0F * m_contentScale);
  if (m_visualizer != nullptr) {
    m_visualizer->setPosition(0.0F, 0.0F);
    m_visualizer->setSize(size, size);
    m_visualizer->setCornerRadius(Style::scaledRadiusLg(m_contentScale));
  }
  setSize(size, size);
}
