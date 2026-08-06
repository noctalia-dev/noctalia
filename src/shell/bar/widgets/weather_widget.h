#pragma once

#include "shell/bar/widget.h"

#include <cstdint>
#include <string>

class Glyph;
class InputArea;
class Label;
class Renderer;
class WeatherService;
struct wl_output;

class WeatherWidget : public Widget {
public:
  struct Options {
    int maxWidth = 160;
    bool showCondition = true;
    bool showTemperature = true;
  };

  WeatherWidget(WeatherService* weather, wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void sync(Renderer& renderer);

  WeatherService* m_weather = nullptr;
  float m_maxWidth = 160.0F;
  bool m_showCondition = true;
  bool m_showTemperature = true;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  std::string m_lastText;
  std::string m_lastGlyph;
  bool m_isVertical = false;
};
