#pragma once

#include "shell/bar/widget.h"

#include <string>

class Label;

class TextWidget : public Widget {
public:
  explicit TextWidget(std::string text);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;

  std::string m_text;
  Label* m_label = nullptr;
};
