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
  void setReversed(bool reversed);
  void setCentered(bool centered);
  void setWave(bool wave);
  void setSmoothingTimeMs(float tauMs) noexcept { m_smoothingTauMs = std::max(0.0F, tauMs); }

  void tick(float deltaMs);
  // Per-frame entry point for widgets. While audio plays it ticks only when setValues() changed
  // the targets, so redraws follow [audio].spectrum_frame_rate rather than the output's refresh
  // rate; skipped frames add their time to the next tick. Once every target is at rest it eases
  // on every frame until settled, since no further values arrive. Returns whether it ticked.
  bool advance(float deltaMs, bool audioActive);
  [[nodiscard]] bool converged() const noexcept { return m_converged; }

private:
  void syncPalette();
  void doLayout(Renderer& renderer) override;

  std::vector<float> m_targetValues;
  std::vector<float> m_displayValues;
  float m_smoothingTauMs = 60.0F;
  float m_msSinceTick = 0.0F;
  bool m_converged = true;
  bool m_newValues = false;
  bool m_valuesAtRest = true;
  ColorSpec m_color1 = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_color2 = colorSpecFromRole(ColorRole::Primary);
  Signal<>::ScopedConnection m_paletteConn;
};
