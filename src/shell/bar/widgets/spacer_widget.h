#pragma once

#include "shell/bar/widget.h"

class SpacerWidget : public Widget {
public:
  struct Options {
    int length = 20;
    bool expand = false;
  };

  SpacerWidget(bool verticalBar, Options options);

  void create() override;

  bool noGapAroundMe() const noexcept override { return true; }
  bool isBarClickThrough() const noexcept override { return true; }
  bool expandsInLane() const noexcept override { return m_expand; }

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  float m_fixedLength;
  bool m_verticalBar = false;
  bool m_expand = false;
};
