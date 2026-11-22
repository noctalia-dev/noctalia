#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"

#include <array>
#include <string>
#include <vector>

class Glyph;
class Label;
class DesktopWeather : public Node {
public:
  struct Forecast {
    std::string day;
    std::string glyph;
    std::string temperatures;
    bool operator==(const Forecast&) const = default;
  };
  struct Data {
    std::string glyph = "weather-cloud";
    std::string temperature = "--";
    std::string condition;
    std::vector<Forecast> forecast;
    bool operator==(const Data&) const = default;
  };
  DesktopWeather();
  bool setData(Data data);
  void setColor(const ColorSpec& color);
  void setShadow(bool enabled);
  void setForecast(bool show, int days);
  void setFontFamily(const std::string& family);
  void setContentScale(float scale);
  static constexpr std::size_t kMaxForecastRows = 6;

private:
  struct ForecastRow {
    Glyph* glyph = nullptr;
    Label* day = nullptr;
    Label* temps = nullptr;
  };
  void doLayout(Renderer& renderer) override;
  void applyShadow();
  void syncData();
  ColorSpec m_color = colorSpecFromRole(ColorRole::OnSurface);
  bool m_shadow = true;
  bool m_showForecast = false;
  int m_forecastDays = 3;
  float m_contentScale = 1.0F;
  Glyph* m_glyph = nullptr;
  Label* m_temperature = nullptr;
  Label* m_condition = nullptr;
  std::array<ForecastRow, kMaxForecastRows> m_forecastRows{};
  Data m_data;
};
