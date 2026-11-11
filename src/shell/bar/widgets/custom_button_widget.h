#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <string>

class Glyph;
class Image;
class InputArea;
class Label;

class CustomButtonWidget : public Widget {
public:
  struct Options {
    std::string glyph = "heart";
    std::string customImage;
    bool customImageColorize = false;
    std::string label;
    std::string tooltip;
    GlyphPosition glyphPosition = GlyphPosition::Before;
    bool peek = false;
    int peekDelayMs = kBarRevealDelayMs;
  };

  explicit CustomButtonWidget(const Options& options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;

  std::string m_glyphName;
  std::string m_labelText;
  std::string m_tooltip;
  WidgetCustomImage m_customImage;
  GlyphPosition m_glyphPosition = GlyphPosition::Before;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Label* m_label = nullptr;
};
