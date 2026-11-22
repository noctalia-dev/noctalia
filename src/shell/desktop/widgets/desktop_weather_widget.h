#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"
#include "weather_widget.h"

#include <array>
#include <cstddef>
#include <string>

class WeatherService;

class DesktopWeatherWidget : public DesktopWidget {
public:
  struct Options {
    ColorSpec color = colorSpecFromRole(ColorRole::OnSurface);
    bool shadow = true;
    bool showForecast = false;
    int forecastDays = 3;
  };

  DesktopWeatherWidget(const WeatherService* weather, Options options);

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  bool sync();
  bool syncForecast();

  const WeatherService* m_weather = nullptr;
  ColorSpec m_color;
  bool m_shadow;
  bool m_showForecast;
  int m_forecastDays;

  DesktopWeather* m_widget = nullptr;
  DesktopWeather::Data m_data;
};
