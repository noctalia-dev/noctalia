#pragma once

#include "render/scene/node.h"
#include "visuals/fancy_audio_visualizer.h"

class DesktopFancyAudioVisualizer : public Node {
public:
  DesktopFancyAudioVisualizer();
  FancyAudioVisualizer& visualizer() noexcept { return *m_visualizer; }
  void setContentScale(float scale);

private:
  void doLayout(Renderer& renderer) override;
  FancyAudioVisualizer* m_visualizer = nullptr;
  float m_contentScale = 1.0F;
};
