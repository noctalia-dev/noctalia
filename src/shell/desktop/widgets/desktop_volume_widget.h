#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <cstdint>
#include <string>

class ConfigService;
class Glyph;
class InputArea;
class Label;
class Node;
class PipeWireService;
struct AudioNode;

class DesktopVolumeWidget : public DesktopWidget {
public:
  struct Options {
    std::string glyph;
    ColorSpec fillColor = colorSpecFromRole(ColorRole::Primary);
    ColorSpec trackColor = colorSpecFromRole(ColorRole::OnSurfaceVariant);
    // false = default sink (output), true = default source (input / mic).
    bool input = false;
    bool showDevice = true;
    bool shadow = true;
    int scrollStepPercent = 5;
    ConfigService* config = nullptr;
  };

  DesktopVolumeWidget(PipeWireService* audio, Options options);

  void create() override;
  void layout(Renderer& renderer) override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;

  void syncState(Renderer& renderer, bool forceLayout);
  void applyColors();
  void applyShadow();
  void updateFillClip();
  [[nodiscard]] const AudioNode* currentNode() const noexcept;
  [[nodiscard]] std::string resolveGlyphName(float volume, bool muted) const;
  [[nodiscard]] float maxVolume() const;

  PipeWireService* m_audio = nullptr;
  ConfigService* m_config = nullptr;
  std::string m_glyphOverride;
  ColorSpec m_fillColor;
  ColorSpec m_trackColor;
  bool m_input = false;
  bool m_showDevice = true;
  bool m_shadow = true;
  float m_scrollStep = 0.05f;
  float m_glyphBox = 0.0f;

  InputArea* m_area = nullptr;
  InputArea* m_glyphHit = nullptr;
  Label* m_percentLabel = nullptr;
  Label* m_statusLabel = nullptr;
  Label* m_deviceLabel = nullptr;
  Glyph* m_trackGlyph = nullptr;
  Node* m_fillClip = nullptr;
  Glyph* m_fillGlyph = nullptr;

  float m_fillProgress = 0.0f;
  std::string m_lastGlyphName;
  std::string m_lastPercentText;
  std::string m_lastStatusText;
  std::string m_lastDeviceText;
  bool m_lastMuted = false;
};
