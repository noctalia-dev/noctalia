#pragma once

#include "shell/bar/widget.h"

class BrightnessService;
class Glyph;
class Label;
struct wl_output;

class BrightnessWidget : public Widget {
public:
  struct Options {
    bool enableScroll = true;
    int scrollStepPercent = 5;
    bool showLabel = true;
  };

  BrightnessWidget(BrightnessService* brightness, wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);

  BrightnessService* m_brightness = nullptr;
  wl_output* m_output = nullptr;
  bool m_showLabel = true;
  bool m_enableScroll = true;
  float m_scrollStep = 0.05f;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  bool m_lastAvailable = false;
  float m_lastBrightness = -1.0f;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
