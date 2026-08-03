#pragma once

#ifndef NDEBUG

#include "shell/bar/widget.h"

class InputArea;
class Glyph;
class Label;

class DebugIndicatorWidget : public Widget {
public:
  DebugIndicatorWidget();

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;

  InputArea* m_container = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
};

#endif
