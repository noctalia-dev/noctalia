#pragma once

#include "render/scene/node.h"

#include <cstddef>

class GridView : public Node {
public:
  void setColumns(std::size_t columns);
  void setColumnGap(float gap);
  void setRowGap(float gap);
  void setPadding(float top, float right, float bottom, float left);
  void setPadding(float all);
  void setPadding(float vertical, float horizontal);
  void setStretchItems(bool stretch);
  void setUniformCellSize(bool uniform);
  // When true with uniform + stretch + fixed height, cells are square (min of slot W/H when width is fixed).
  void setSquareCells(bool square);
  // When true (default), square grids with no fixed width shrink-wrap to the tile block; when false, width fills the
  // flex slot (wider tiles; leftover space stays on the right of left-aligned tiles).
  void setSquareGridShrinkWrap(bool shrinkWrap);
  void setMinCellWidth(float width);
  void setMinCellHeight(float height);

  [[nodiscard]] std::size_t columns() const noexcept { return m_columns; }
  [[nodiscard]] float columnGap() const noexcept { return m_columnGap; }
  [[nodiscard]] float rowGap() const noexcept { return m_rowGap; }
  [[nodiscard]] float paddingTop() const noexcept { return m_paddingTop; }
  [[nodiscard]] float paddingRight() const noexcept { return m_paddingRight; }
  [[nodiscard]] float paddingBottom() const noexcept { return m_paddingBottom; }
  [[nodiscard]] float paddingLeft() const noexcept { return m_paddingLeft; }
  [[nodiscard]] bool stretchItems() const noexcept { return m_stretchItems; }
  [[nodiscard]] bool uniformCellSize() const noexcept { return m_uniformCellSize; }
  [[nodiscard]] bool squareCells() const noexcept { return m_squareCells; }
  [[nodiscard]] bool squareGridShrinkWrap() const noexcept { return m_squareGridShrinkWrap; }
  [[nodiscard]] float minCellWidth() const noexcept { return m_minCellWidth; }
  [[nodiscard]] float minCellHeight() const noexcept { return m_minCellHeight; }

private:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;
  std::size_t m_columns = 1;
  float m_columnGap = 0.0f;
  float m_rowGap = 0.0f;
  float m_paddingTop = 0.0f;
  float m_paddingRight = 0.0f;
  float m_paddingBottom = 0.0f;
  float m_paddingLeft = 0.0f;
  bool m_stretchItems = false;
  bool m_uniformCellSize = true;
  bool m_squareCells = false;
  bool m_squareGridShrinkWrap = true;
  float m_minCellWidth = 0.0f;
  float m_minCellHeight = 0.0f;
};
