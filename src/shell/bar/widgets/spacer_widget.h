#pragma once

#include "shell/bar/widget.h"

class SpacerWidget : public Widget {
public:
  explicit SpacerWidget(float length = 0.0f, bool verticalBar = false);

  void create() override;

  bool noGapAroundMe() const noexcept override { return true; }
  bool isBarClickThrough() const noexcept override { return true; }

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  float m_fixedLength;
  bool m_verticalBar = false;
};
