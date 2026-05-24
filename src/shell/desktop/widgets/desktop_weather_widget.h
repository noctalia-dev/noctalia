#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <string>

class Glyph;
class Label;
class WeatherService;

class DesktopWeatherWidget : public DesktopWidget {
public:
  DesktopWeatherWidget(const WeatherService* weather, ColorSpec color, bool shadow);

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  bool sync();
  void applyShadow();

  const WeatherService* m_weather = nullptr;
  ColorSpec m_color;
  bool m_shadow;

  Glyph* m_glyph = nullptr;
  Label* m_temperature = nullptr;
  Label* m_condition = nullptr;

  std::string m_lastGlyph;
  std::string m_lastTemperature;
  std::string m_lastCondition;
};
