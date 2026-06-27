#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <cstdint>

class AudioVisualizer;
class PipeWireSpectrum;
class Renderer;

class DesktopAudioVisualizerWidget : public DesktopWidget {
public:
  struct Options {
    int bands = 32;
    bool mirrored = true;
    bool centered = true;
    bool showWhenIdle = true;
    ColorSpec color1 = colorSpecFromRole(ColorRole::Primary);
    ColorSpec color2 = colorSpecFromRole(ColorRole::Primary);
  };

  DesktopAudioVisualizerWidget(PipeWireSpectrum* spectrum, Options options);
  ~DesktopAudioVisualizerWidget() override;

  void create() override;
  void layout(Renderer& renderer) override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;
  void setEditorPreview(bool enabled) noexcept override;
  [[nodiscard]] bool needsFrameTick() const override;
  void onFrameTick(float deltaMs, Renderer& renderer) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void pullSpectrumValues();
  void syncSpectrum(Renderer* renderer);
  void layoutContentSize(Renderer& renderer);
  [[nodiscard]] bool shouldBeVisible() const;
  bool applyVisibility();
  void cancelVisibilityAnimation();
  void setVisibilityCollapsed(bool collapsed);
  void startOpacityAnimation(float targetOpacity, bool collapseOnComplete);

  PipeWireSpectrum* m_spectrum = nullptr;
  int m_bands = 32;
  bool m_mirrored = true;
  bool m_centered = true;
  bool m_showWhenIdle = false;
  bool m_editorPreview = false;
  ColorSpec m_color1 = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_color2 = colorSpecFromRole(ColorRole::Primary);
  std::uint64_t m_listenerId = 0;
  AudioVisualizer* m_visualizer = nullptr;
  bool m_pendingSpectrumUpdate = false;
  bool m_visible = true;
  bool m_visibilityInitialized = false;
  bool m_fadingOut = false;
  std::uint32_t m_visibilityAnimId = 0;
};
