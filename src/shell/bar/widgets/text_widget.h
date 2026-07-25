#pragma once

#include "shell/bar/widget.h"

#include <string>

class Label;

class TextWidget : public Widget {
public:
  struct Options {
    std::string text;

    bool operator==(const Options&) const = default;
  };

  explicit TextWidget(Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;

  std::string m_text;
  Label* m_label = nullptr;
};
