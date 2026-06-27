#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <cstdint>
#include <string>

class Label;
class Node;
class RectNode;

class DesktopClockWidget : public DesktopWidget {
public:
  enum class Style : std::uint8_t {
    Digital,
    Analog,
  };

  struct Options {
    Style style = Style::Digital;
    std::string format;
    ColorSpec color = colorSpecFromRole(ColorRole::OnSurface);
    bool shadow = true;
    bool showCircle = true;
    bool centerText = false;
  };

  explicit DesktopClockWidget(Options options);

  void create() override;
  [[nodiscard]] bool wantsSecondTicks() const override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  [[nodiscard]] std::string formatText() const;
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  void applyShadow();
  void syncStyleVisibility();
  void syncCircleVisibility();
  void syncDigitalTextAlign();
  void syncAnalogColors();
  void layoutAnalog(Renderer& renderer, float size);
  void layoutDigital(Renderer& renderer);
  // Pin the digital label to the width of its widest-digit rendering so the box
  // stops reflowing every second as proportional digits change advance width.
  void updateStableDigitalWidth(Renderer& renderer, const std::string& text);
  void updateAnalogHands();
  [[nodiscard]] static Style styleFromSetting(std::string_view value);

  Style m_style;
  std::string m_format;
  ColorSpec m_color;
  bool m_shadow;
  bool m_showCircle;
  bool m_centerText = false;
  bool m_showsSeconds = false;
  Label* m_label = nullptr;
  Node* m_digitalRoot = nullptr;
  Node* m_analogRoot = nullptr;
  Node* m_ticksRoot = nullptr;
  Node* m_hourPivot = nullptr;
  Node* m_minutePivot = nullptr;
  Node* m_secondPivot = nullptr;
  RectNode* m_face = nullptr;
  RectNode* m_hub = nullptr;
  std::string m_lastText;
  int m_lastHour = -1;
  int m_lastMinute = -1;
  int m_lastSecond = -1;

  // Stable-width state for the digital label. m_stableSample is the current text
  // with every digit replaced by the widest digit glyph; its measured width is
  // constant across seconds, so we only re-measure when it actually changes.
  float m_stableWidth = 0.0f;
  std::string m_stableSample;
  char m_widestDigit = '0';
  float m_metricsFontSize = -1.0f;
  std::string m_metricsFontFamily;
  // Horizontal centering for proportional digits: the label is start-aligned (leading glyph
  // anchored) but shifted right by m_digitOffsetX so the *mean*-width rendering sits centered in
  // the reserved box, instead of all the widest-digit slack piling up on the trailing side.
  // m_digitOffsetX = digitCount * (widestAdvance - meanDigitAdvance); 0 for equal-width digits.
  float m_maxDigitAdvance = 0.0f;
  float m_meanDigitAdvance = 0.0f;
  float m_digitOffsetX = 0.0f;
};
