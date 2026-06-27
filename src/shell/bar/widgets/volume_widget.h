#pragma once

#include "shell/bar/widget.h"

#include <cstdint>
#include <string>

struct Config;
class EasyEffectsService;
class Glyph;
class Label;
class PipeWireService;
struct wl_output;

enum class VolumeWidgetTarget {
  Output,
  Input,
};

class VolumeWidget : public Widget {
public:
  VolumeWidget(
      PipeWireService* audio, EasyEffectsService* easyEffects, const Config* config, wl_output* output, bool showLabel,
      VolumeWidgetTarget target, int scrollStepPercent, ColorSpec muteColor, std::string glyphOverride,
      std::string muteGlyphOverride
  );

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);
  [[nodiscard]] std::string glyphName(float volume, bool muted) const;

  PipeWireService* m_audio = nullptr;
  EasyEffectsService* m_easyEffects = nullptr;
  const Config* m_config = nullptr;
  bool m_showLabel = true;
  float m_scrollStep = 0.05f;
  VolumeWidgetTarget m_target = VolumeWidgetTarget::Output;
  ColorSpec m_muteColor;
  std::string m_glyphOverride;
  std::string m_muteGlyphOverride;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  float m_lastVolume = -1.0f;
  std::string m_lastEffectsProfile;
  bool m_lastMuted = false;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
