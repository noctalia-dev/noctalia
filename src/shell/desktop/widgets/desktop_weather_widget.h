#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <array>
#include <cstddef>
#include <string>

class Glyph;
class Label;
class Renderer;
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
  struct ForecastRow {
    Glyph* glyph = nullptr;
    Label* day = nullptr;
    Label* temps = nullptr;
    std::string lastDay;
    std::string lastGlyph;
    std::string lastTemps;
  };

  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  bool sync();
  bool syncForecast(Renderer& renderer);
  void applyShadow();

  static constexpr std::size_t kMaxForecastRows = 6;

  const WeatherService* m_weather = nullptr;
  ColorSpec m_color;
  bool m_shadow;
  bool m_showForecast;
  int m_forecastDays;

  Glyph* m_glyph = nullptr;
  Label* m_temperature = nullptr;
  Label* m_condition = nullptr;
  std::array<ForecastRow, kMaxForecastRows> m_forecastRows{};

  std::string m_lastGlyph;
  std::string m_lastTemperature;
  std::string m_lastCondition;
};
