#include "audio_visualizer_widget.h"

#include "ui/style.h"

#include <cmath>
#include <memory>

DesktopAudioVisualizer::DesktopAudioVisualizer() {
  setClipChildren(true);
  auto visualizer = std::make_unique<AudioVisualizer>();
  visualizer->setOrientation(AudioSpectrumOrientation::Horizontal);
  m_visualizer = visualizer.get();
  addChild(std::move(visualizer));
}

void DesktopAudioVisualizer::setContentScale(float scale) {
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

void DesktopAudioVisualizer::setViewport(float width, float height) {
  if (m_viewWidth == width && m_viewHeight == height) {
    return;
  }
  m_viewWidth = width;
  m_viewHeight = height;
  markLayoutDirty();
}

void DesktopAudioVisualizer::doLayout(Renderer& /*renderer*/) {
  float width = m_viewWidth;
  float height = m_viewHeight;
  if (width <= 0.0F || height <= 0.0F) {
    width = 240.0F * m_contentScale;
    height = 96.0F * m_contentScale;
  }
  if (m_visualizer != nullptr) {
    m_visualizer->setPosition(0.0F, 0.0F);
    m_visualizer->setSize(width, height);
  }
  setSize(width, height);
}
