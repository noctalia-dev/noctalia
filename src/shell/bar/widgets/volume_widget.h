#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <cstdint>
#include <string>
#include <unordered_map>

struct Config;
class EasyEffectsService;
class Glyph;
class Image;
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
      std::string muteGlyphOverride, std::unordered_map<std::string, std::string> effectsProfileGlyphs,
      WidgetCustomImage customImage = {}, bool enableScroll = true
  );

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);
  [[nodiscard]] std::string glyphName(float volume, bool muted, const std::string& effectsProfile = {}) const;

  PipeWireService* m_audio = nullptr;
  EasyEffectsService* m_easyEffects = nullptr;
  const Config* m_config = nullptr;
  bool m_showLabel = true;
  bool m_enableScroll = true;
  float m_scrollStep = 0.05f;
  VolumeWidgetTarget m_target = VolumeWidgetTarget::Output;
  ColorSpec m_muteColor;
  std::string m_glyphOverride;
  std::string m_muteGlyphOverride;
  std::unordered_map<std::string, std::string> m_effectsProfileGlyphs;
  WidgetCustomImage m_customImage;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Label* m_label = nullptr;
  float m_lastVolume = -1.0f;
  std::string m_lastEffectsProfile;
  bool m_lastMuted = false;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
