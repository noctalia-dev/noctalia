#include "ui/controls/grid_view.h"

#include "render/core/renderer.h"

#include <algorithm>
#include <numeric>
#include <vector>

void GridView::setColumns(std::size_t columns) {
  const std::size_t normalized = std::max<std::size_t>(1, columns);
  if (m_columns == normalized) {
    return;
  }
  m_columns = normalized;
  markLayoutDirty();
}

void GridView::setColumnGap(float gap) {
  if (m_columnGap == gap && m_rowGap == gap) {
    return;
  }
  m_columnGap = gap;
  m_rowGap = gap;
  markLayoutDirty();
}

void GridView::setRowGap(float gap) {
  // GridView keeps row/column spacing symmetric by design.
  setColumnGap(gap);
}

void GridView::setPadding(float top, float right, float bottom, float left) {
  m_paddingTop = top;
  m_paddingRight = right;
  m_paddingBottom = bottom;
  m_paddingLeft = left;
  markLayoutDirty();
}

void GridView::setPadding(float all) { setPadding(all, all, all, all); }

void GridView::setPadding(float vertical, float horizontal) { setPadding(vertical, horizontal, vertical, horizontal); }

void GridView::setStretchItems(bool stretch) {
  if (m_stretchItems == stretch) {
    return;
  }
  m_stretchItems = stretch;
  markLayoutDirty();
}

void GridView::setUniformCellSize(bool uniform) {
  if (m_uniformCellSize == uniform) {
    return;
  }
  m_uniformCellSize = uniform;
  markLayoutDirty();
}

void GridView::setSquareCells(bool square) {
  if (m_squareCells == square) {
    return;
  }
  m_squareCells = square;
  markLayoutDirty();
}

void GridView::setSquareGridShrinkWrap(bool shrinkWrap) {
  if (m_squareGridShrinkWrap == shrinkWrap) {
    return;
  }
  m_squareGridShrinkWrap = shrinkWrap;
  markLayoutDirty();
}

void GridView::setMinCellWidth(float width) {
  const float normalized = std::max(0.0f, width);
  if (m_minCellWidth == normalized) {
    return;
  }
  m_minCellWidth = normalized;
  markLayoutDirty();
}

void GridView::setMinCellHeight(float height) {
  const float normalized = std::max(0.0f, height);
  if (m_minCellHeight == normalized) {
    return;
  }
  m_minCellHeight = normalized;
  markLayoutDirty();
}

LayoutSize GridView::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  float useW = width();
  if (constraints.hasExactWidth()) {
    useW = constraints.maxWidth;
  } else if (m_squareCells && m_stretchItems && m_squareGridShrinkWrap && constraints.hasExactHeight()) {
    // Intrinsic width from square row height (shrink-wrapped square grid only).
    useW = 0.0f;
  }
  float useH = height();
  if (constraints.hasExactHeight()) {
    useH = constraints.maxHeight;
  }
  if (constraints.hasExactWidth() || constraints.hasExactHeight()) {
    setSize(useW, useH);
  }
  doLayout(renderer);
  return constraints.constrain(LayoutSize{.width = width(), .height = height()});
}

void GridView::doLayout(Renderer& renderer) {
  auto layoutWithAssignedSize = [&renderer](Node* child, float assignedWidth, float assignedHeight) {
    child->arrange(
        renderer, LayoutRect{.x = child->x(), .y = child->y(), .width = assignedWidth, .height = assignedHeight}
    );
  };

  std::vector<Node*> visibleChildren;
  visibleChildren.reserve(children().size());
  for (auto& child : children()) {
    if (child->visible()) {
      visibleChildren.push_back(child.get());
    }
  }

  if (visibleChildren.empty()) {
    setSize(std::max(width(), m_paddingLeft + m_paddingRight), std::max(height(), m_paddingTop + m_paddingBottom));
    return;
  }

  const std::size_t columns = std::min(m_columns, std::max<std::size_t>(1, visibleChildren.size()));
  const std::size_t rows = (visibleChildren.size() + columns - 1) / columns;

  const bool hasFixedWidth = width() > 0.0f;
  const bool hasFixedHeight = height() > 0.0f;
  const float fixedWidth = width();
  const float fixedHeight = height();

  std::vector<float> columnWidths(columns, 0.0f);
  std::vector<float> rowHeights(rows, 0.0f);

  float stretchedWidth = 0.0f;
  if (hasFixedWidth && m_stretchItems && columns > 0) {
    const float innerWidth =
        std::max(0.0f, fixedWidth - m_paddingLeft - m_paddingRight - m_columnGap * static_cast<float>(columns - 1));
    stretchedWidth = innerWidth / static_cast<float>(columns);
  }

  if (m_uniformCellSize) {
    // Pass 1: measure natural child sizes without imposed cell width.
    float maxMeasuredWidth = 0.0f;
    float maxMeasuredHeight = 0.0f;
    for (Node* child : visibleChildren) {
      child->layout(renderer);
      maxMeasuredWidth = std::max(maxMeasuredWidth, child->width());
      maxMeasuredHeight = std::max(maxMeasuredHeight, child->height());
    }

    float uniformWidth = maxMeasuredWidth;
    float uniformHeight = std::max(maxMeasuredHeight, m_minCellHeight);

    const bool squarePack = m_squareCells && m_stretchItems && columns > 0 && rows > 0 && hasFixedHeight;
    if (squarePack) {
      const float innerHeight = std::max(
          0.0f, fixedHeight - m_paddingTop - m_paddingBottom - m_rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0)
      );
      const float slotH = innerHeight / static_cast<float>(rows);
      if (hasFixedWidth) {
        const float innerWidth =
            std::max(0.0f, fixedWidth - m_paddingLeft - m_paddingRight - m_columnGap * static_cast<float>(columns - 1));
        const float slotW = innerWidth / static_cast<float>(columns);
        uniformWidth = uniformHeight = std::min(slotW, slotH);
      } else {
        // Shrink-wrapped grid: width comes from content; height drives square side.
        uniformWidth = uniformHeight = slotH;
      }
    } else {
      if (hasFixedWidth && columns > 0) {
        const float innerWidth =
            std::max(0.0f, fixedWidth - m_paddingLeft - m_paddingRight - m_columnGap * static_cast<float>(columns - 1));
        const float slotW = innerWidth / static_cast<float>(columns);
        if (m_stretchItems) {
          uniformWidth = slotW;
        } else {
          uniformWidth = std::max({uniformWidth, m_minCellWidth, slotW});
        }
      } else if (hasFixedWidth && m_stretchItems && stretchedWidth > 0.0f) {
        uniformWidth = std::max(uniformWidth, stretchedWidth);
      } else {
        uniformWidth = std::max(uniformWidth, m_minCellWidth);
      }

      if (hasFixedHeight && rows > 0) {
        const float innerHeight = std::max(
            0.0f, fixedHeight - m_paddingTop - m_paddingBottom - m_rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0)
        );
        const float slotH = innerHeight / static_cast<float>(rows);
        if (m_stretchItems) {
          uniformHeight = slotH;
        } else {
          uniformHeight = std::max({uniformHeight, m_minCellHeight, slotH});
        }
      }
    }

    std::fill(columnWidths.begin(), columnWidths.end(), uniformWidth);
    std::fill(rowHeights.begin(), rowHeights.end(), uniformHeight);

    for (Node* child : visibleChildren) {
      layoutWithAssignedSize(child, uniformWidth, uniformHeight);
    }
  } else {
    if (hasFixedWidth && m_stretchItems) {
      std::fill(columnWidths.begin(), columnWidths.end(), stretchedWidth);
    }

    for (std::size_t index = 0; index < visibleChildren.size(); ++index) {
      Node* child = visibleChildren[index];
      const std::size_t col = index % columns;
      const std::size_t row = index / columns;

      if (hasFixedWidth && m_stretchItems && stretchedWidth > 0.0f) {
        layoutWithAssignedSize(child, columnWidths[col], child->height());
      } else {
        child->layout(renderer);
      }

      if (!hasFixedWidth || !m_stretchItems) {
        columnWidths[col] = std::max(columnWidths[col], child->width());
      }
      rowHeights[row] = std::max(rowHeights[row], child->height());
    }

    for (auto& columnWidth : columnWidths) {
      columnWidth = std::max(columnWidth, m_minCellWidth);
    }
    for (auto& rowHeight : rowHeights) {
      rowHeight = std::max(rowHeight, m_minCellHeight);
    }
  }

  const float contentWidth = std::max(
      0.0f, std::accumulate(columnWidths.begin(), columnWidths.end(), 0.0f) +
                m_columnGap * static_cast<float>(columns > 0 ? columns - 1 : 0)
  );
  const float contentHeight = std::max(
      0.0f, std::accumulate(rowHeights.begin(), rowHeights.end(), 0.0f) +
                m_rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0)
  );

  const float computedWidth = m_paddingLeft + contentWidth + m_paddingRight;
  const float computedHeight = m_paddingTop + contentHeight + m_paddingBottom;
  const bool tightSquare = m_squareCells && m_stretchItems && m_uniformCellSize;
  const float outW =
      (tightSquare && m_squareGridShrinkWrap) ? computedWidth : (hasFixedWidth ? fixedWidth : computedWidth);
  const float outH = hasFixedHeight ? fixedHeight : computedHeight;
  setSize(outW, outH);

  float originX = m_paddingLeft;
  float originY = m_paddingTop;
  if (tightSquare && hasFixedHeight && rows > 0) {
    const float usedH = std::accumulate(rowHeights.begin(), rowHeights.end(), 0.0f) +
                        m_rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0);
    const float availH = std::max(0.0f, fixedHeight - m_paddingTop - m_paddingBottom);
    originY = m_paddingTop + std::max(0.0f, (availH - usedH) * 0.5f);
  }

  std::vector<float> columnOffsets(columns, originX);
  for (std::size_t col = 1; col < columns; ++col) {
    columnOffsets[col] = columnOffsets[col - 1] + columnWidths[col - 1] + m_columnGap;
  }

  std::vector<float> rowOffsets(rows, originY);
  for (std::size_t row = 1; row < rows; ++row) {
    rowOffsets[row] = rowOffsets[row - 1] + rowHeights[row - 1] + m_rowGap;
  }

  for (std::size_t index = 0; index < visibleChildren.size(); ++index) {
    Node* child = visibleChildren[index];
    const std::size_t col = index % columns;
    const std::size_t row = index / columns;
    child->setPosition(columnOffsets[col], rowOffsets[row]);
  }
}
