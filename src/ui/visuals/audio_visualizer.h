#pragma once

#include "render/scene/audio_spectrum_node.h"
#include "ui/palette.h"

#include <algorithm>
#include <vector>

class AudioVisualizer : public AudioSpectrumNode {
public:
  AudioVisualizer();

  bool setValues(const std::vector<float>& values);
  void setGradient(const ColorSpec& color1, const ColorSpec& color2);
  void setGradient(const Color& color1, const Color& color2);
  void setOrientation(AudioSpectrumOrientation orientation);
  void setMirrored(bool mirrored);
  void setCentered(bool centered);
  void setSmoothingTimeMs(float tauMs) noexcept { m_smoothingTauMs = std::max(0.0f, tauMs); }

  void tick(float deltaMs);
  [[nodiscard]] bool converged() const noexcept { return m_converged; }

private:
  void syncPalette();
  void doLayout(Renderer& renderer) override;

  std::vector<float> m_targetValues;
  std::vector<float> m_displayValues;
  float m_smoothingTauMs = 60.0f;
  bool m_converged = true;
  ColorSpec m_color1 = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_color2 = colorSpecFromRole(ColorRole::Primary);
  Signal<>::ScopedConnection m_paletteConn;
};
