#pragma once

#include "shell/bar/widget.h"

#include <string>

class Glyph;
class InputArea;
class Label;

class CustomButtonWidget : public Widget {
public:
  struct Options {
    std::string glyph;
    std::string label;
    std::string tooltip;
    std::string command;
    std::string rightCommand;
    std::string middleCommand;
    std::string scrollUpCommand;
    std::string scrollDownCommand;
  };

  explicit CustomButtonWidget(Options options);

  void create() override;
  [[nodiscard]] bool reservesMiddleClick() const noexcept override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void executeCommand(const std::string& command) const;

  std::string m_glyphName;
  std::string m_labelText;
  std::string m_tooltip;
  std::string m_command;
  std::string m_rightCommand;
  std::string m_middleCommand;
  std::string m_scrollUpCommand;
  std::string m_scrollDownCommand;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
};
