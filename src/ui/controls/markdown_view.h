#pragma once

#include "ui/controls/flex.h"

#include <string>
#include <vector>

class Label;

class MarkdownView : public Flex {
public:
  void setMarkdown(const std::string& markdown, float scale);
  void clear();
  void trackWrappableLabel(Label* label) { m_wrappableLabels.push_back(label); }

protected:
  void doLayout(Renderer& renderer) override;

private:
  float m_scale = 1.0f;
  std::vector<Label*> m_wrappableLabels;
};
