#pragma once

#include "render/core/render_styles.h"
#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <cstdint>

class FancyAudioVisualizer;
class PipeWireSpectrum;
class Renderer;

class DesktopFancyAudioVisualizerWidget : public DesktopWidget {
public:
  struct Options {
    FancyAudioVisualizerMode mode = FancyAudioVisualizerMode::BarsRings;
    float sensitivity = 1.5f;
    float rotationSpeed = 0.5f;
    float barWidth = 0.6f;
    float ringOpacity = 0.8f;
    float bloomIntensity = 0.5f;
    float waveThickness = 1.0f;
    float innerDiameter = 0.7f;
    bool fadeWhenIdle = true;
    ColorSpec primaryColor = colorSpecFromRole(ColorRole::Primary);
    ColorSpec secondaryColor = colorSpecFromRole(ColorRole::Secondary);
  };

  DesktopFancyAudioVisualizerWidget(PipeWireSpectrum* spectrum, Options options);
  ~DesktopFancyAudioVisualizerWidget() override;

  void create() override;
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
  void layoutContentSize(Renderer& renderer);
  void ensureSpectrumTexture(Renderer& renderer);
  void pullSpectrumValues(Renderer& renderer);
  void syncSpectrum(Renderer& renderer);
  void syncStyle();
  [[nodiscard]] bool shouldBeVisible() const;
  [[nodiscard]] bool shouldAnimateTime() const;
  bool applyVisibility();
  void cancelVisibilityAnimation();
  void setVisibilityCollapsed(bool collapsed);
  void startOpacityAnimation(float targetOpacity, bool collapseOnComplete);

  PipeWireSpectrum* m_spectrum = nullptr;
  FancyAudioVisualizerMode m_mode;
  float m_sensitivity;
  float m_rotationSpeed;
  float m_barWidth;
  float m_ringOpacity;
  float m_bloomIntensity;
  float m_waveThickness;
  float m_innerDiameter;
  bool m_fadeWhenIdle;
  ColorSpec m_primaryColor;
  ColorSpec m_secondaryColor;

  std::uint64_t m_listenerId = 0;
  FancyAudioVisualizer* m_visualizer = nullptr;
  float m_shaderTime = 0.0f;
  bool m_pendingSpectrumUpdate = false;
  bool m_spectrumTextureInitialized = false;
  bool m_editorPreview = false;
  bool m_visible = true;
  bool m_visibilityInitialized = false;
  bool m_fadingOut = false;
  std::uint32_t m_visibilityAnimId = 0;
};
