#pragma once

#include "ui/controls/flex.h"

// Flex that fills its GridView cell. Layout temporarily raises min size to the cell,
// then clears mins to zero (do not rely on persistent setMinWidth/setMinHeight here).
class GridTile : public Flex {
public:
protected:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
};
