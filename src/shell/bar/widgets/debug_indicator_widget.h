#pragma once

#ifndef NDEBUG

#include "shell/bar/widget.h"

class Flex;
class Glyph;
class Label;

class DebugIndicatorWidget : public Widget {
public:
  DebugIndicatorWidget();

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;

  Flex* m_container = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
};

#endif
