#include "ui/controls/grid_tile.h"

#include <algorithm>

void GridTile::doLayout(Renderer& renderer) {
  const float aw = width();
  const float ah = height();
  if (aw > 0.0f) {
    Flex::setMinWidth(aw);
  }
  if (ah > 0.0f) {
    Flex::setMinHeight(ah);
  }
  Flex::doLayout(renderer);
  Flex::setMinWidth(0.0f);
  Flex::setMinHeight(0.0f);
  if (aw > 0.0f || ah > 0.0f) {
    Flex::setSize(std::max(width(), aw), std::max(height(), ah));
  }
}

LayoutSize GridTile::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void GridTile::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }
