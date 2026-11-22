#pragma once

#include "render/scene/node.h"
#include "visuals/audio_visualizer.h"

class DesktopAudioVisualizer : public Node {
public:
  DesktopAudioVisualizer();
  AudioVisualizer& visualizer() noexcept { return *m_visualizer; }
  void setContentScale(float scale);
  void setViewport(float width, float height);

private:
  void doLayout(Renderer& renderer) override;
  AudioVisualizer* m_visualizer = nullptr;
  float m_contentScale = 1.0F;
  float m_viewWidth = 0.0F;
  float m_viewHeight = 0.0F;
};
