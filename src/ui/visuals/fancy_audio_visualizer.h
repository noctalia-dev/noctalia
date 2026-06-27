#pragma once

#include "render/scene/fancy_audio_visualizer_node.h"
#include "ui/palette.h"

#include <span>
#include <vector>

class TextureManager;

class FancyAudioVisualizer : public FancyAudioVisualizerNode {
public:
  FancyAudioVisualizer();

  bool setValues(TextureManager& textures, std::span<const float> values);
  bool setValues(TextureManager& textures, const std::vector<float>& values);
  void setVisualizationMode(FancyAudioVisualizerMode mode);
  void setPrimaryColor(const ColorSpec& color);
  void setSecondaryColor(const ColorSpec& color);
  void setSensitivity(float sensitivity);
  void setRotationSpeed(float speed);
  void setBarWidth(float width);
  void setRingOpacity(float opacity);
  void setBloomIntensity(float intensity);
  void setWaveThickness(float thickness);
  void setInnerDiameter(float diameter);
  void setCornerRadius(float radius);
  void setTime(float time);

private:
  void syncPalette();
  void doLayout(Renderer& renderer) override;

  ColorSpec m_primaryColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_secondaryColor = colorSpecFromRole(ColorRole::Secondary);
  Signal<>::ScopedConnection m_paletteConn;
};
