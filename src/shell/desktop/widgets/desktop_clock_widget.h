#pragma once

#include "render/core/renderer.h"
#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <cstdint>
#include <string>

class DesktopClock;

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
    bool showSeconds = true;
    bool centerText = true;
    std::string timezone;
    float fontSize = 0;
    FontWeight fontWeight = FontWeight::Bold;
    bool tightText = false;
  };

  explicit DesktopClockWidget(Options options);

  void create() override;
  bool wantsSecondTicks() const override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  std::string formatText() const;
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  bool contentAlignsToStart() const noexcept override { return !m_centerText; }
  void updateAnalogTime();
  static Style styleFromSetting(std::string_view value);

  Style m_style;
  std::string m_format;
  ColorSpec m_color;
  bool m_shadow;
  bool m_showCircle;
  bool m_showSeconds;
  std::string m_timezone;
  bool m_centerText = true;
  bool m_showsSeconds = false;
  DesktopClock* m_clock = nullptr;
  float m_fontSize = 0;
  FontWeight m_fontWeight = FontWeight::Bold;
  bool m_tightText = false;
};
