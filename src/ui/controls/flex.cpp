#include "ui/controls/flex.h"

#include "render/core/render_styles.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <vector>

namespace {

  float mainSize(LayoutSize size, bool horizontal) { return horizontal ? size.width : size.height; }

  float crossSize(LayoutSize size, bool horizontal) { return horizontal ? size.height : size.width; }

  float mainPaddingStart(const Flex& flex, bool horizontal) {
    return horizontal ? flex.paddingLeft() : flex.paddingTop();
  }

  float mainPaddingEnd(const Flex& flex, bool horizontal) {
    return horizontal ? flex.paddingRight() : flex.paddingBottom();
  }

  float crossPaddingStart(const Flex& flex, bool horizontal) {
    return horizontal ? flex.paddingTop() : flex.paddingLeft();
  }

  float crossPaddingEnd(const Flex& flex, bool horizontal) {
    return horizontal ? flex.paddingBottom() : flex.paddingRight();
  }

  void setMainConstraint(LayoutConstraints& constraints, bool horizontal, float value) {
    if (horizontal) {
      constraints.setExactWidth(value);
    } else {
      constraints.setExactHeight(value);
    }
  }

  void setCrossConstraint(LayoutConstraints& constraints, bool horizontal, float value) {
    if (horizontal) {
      constraints.setExactHeight(value);
    } else {
      constraints.setExactWidth(value);
    }
  }

  // Soft cross-axis upper bound. Lets children (esp. labels) discover a wrap budget
  // even when align != Stretch, without forcing them to fill the full cross extent.
  void setCrossMaxConstraint(LayoutConstraints& constraints, bool horizontal, float value) {
    if (horizontal) {
      constraints.setMaxHeight(value);
    } else {
      constraints.setMaxWidth(value);
    }
  }

  LayoutSize sizeFromAxes(bool horizontal, float main, float cross) {
    return horizontal ? LayoutSize{.width = main, .height = cross} : LayoutSize{.width = cross, .height = main};
  }

  LayoutRect rectFromAxes(bool horizontal, float mainPos, float crossPos, float main, float cross) {
    return horizontal ? LayoutRect{.x = mainPos, .y = crossPos, .width = main, .height = cross}
                      : LayoutRect{.x = crossPos, .y = mainPos, .width = cross, .height = main};
  }

} // namespace

struct Flex::ChildLayout {
  Node* node = nullptr;
  LayoutSize measured{};
  float main = 0.0F;
  float cross = 0.0F;
  bool gapExcluded = false;
};

namespace {

  // Pool of scratch buffers for Flex::runLayout. Layout can recurse (a Flex
  // child triggers another runLayout while the parent's items vector is still
  // live), so we keep a stack indexed by nesting depth. Each call grabs the
  // slot for its depth and clears it on entry; capacity is preserved across
  // calls so steady-state layout does no heap allocation for these vectors.
  // std::deque is used so growing the stack during a recursive call does not
  // invalidate references held by parent frames.
  thread_local std::deque<std::vector<Flex::ChildLayout>> tlScratchStack;
  thread_local std::size_t tlScratchDepth = 0;

  class FlexScratchGuard {
  public:
    FlexScratchGuard() {
      if (tlScratchStack.size() <= tlScratchDepth) {
        tlScratchStack.emplace_back();
      }
      m_slot = &tlScratchStack[tlScratchDepth++];
      m_slot->clear();
    }
    ~FlexScratchGuard() {
      m_slot->clear();
      --tlScratchDepth;
    }

    FlexScratchGuard(const FlexScratchGuard&) = delete;
    FlexScratchGuard& operator=(const FlexScratchGuard&) = delete;

    [[nodiscard]] std::vector<Flex::ChildLayout>& items() noexcept { return *m_slot; }

  private:
    std::vector<Flex::ChildLayout>* m_slot;
  };

} // namespace

Flex::Flex() {
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
}

void Flex::setSize(float width, float height) {
  if (!m_sizingFromLayout && !arrangingByLayout()) {
    m_explicitWidth = width > 0.0F;
    m_explicitHeight = height > 0.0F;
  }
  Node::setSize(width, height);
  if (m_background != nullptr) {
    m_background->setPosition(0.0F, 0.0F);
    m_background->setFrameSize(width, height);
  }
}

void Flex::setFrameSize(float width, float height) {
  if (!m_sizingFromLayout && !arrangingByLayout()) {
    m_explicitWidth = width > 0.0F;
    m_explicitHeight = height > 0.0F;
  }
  Node::setFrameSize(width, height);
  if (m_background != nullptr) {
    m_background->setPosition(0.0F, 0.0F);
    m_background->setFrameSize(width, height);
  }
}

void Flex::setDirection(FlexDirection direction) {
  if (m_direction == direction) {
    return;
  }
  m_direction = direction;
  markLayoutDirty();
}

void Flex::setWrap(bool wrap) {
  if (m_wrap == wrap) {
    return;
  }
  m_wrap = wrap;
  markLayoutDirty();
}

void Flex::setGap(float gap) {
  if (m_gap == gap) {
    return;
  }
  m_gap = gap;
  markLayoutDirty();
}

void Flex::setAlign(FlexAlign align) {
  if (m_align == align) {
    return;
  }
  m_align = align;
  markLayoutDirty();
}

void Flex::setJustify(FlexJustify justify) {
  if (m_justify == justify) {
    return;
  }
  m_justify = justify;
  markLayoutDirty();
}

void Flex::setMirrorInRtl(bool mirror) {
  if (m_mirrorInRtl == mirror) {
    return;
  }
  m_mirrorInRtl = mirror;
  markLayoutDirty();
}

void Flex::setPadding(float top, float right, float bottom, float left) {
  m_paddingTop = top;
  m_paddingRight = right;
  m_paddingBottom = bottom;
  m_paddingLeft = left;
  markLayoutDirty();
}

void Flex::setPadding(float all) { setPadding(all, all, all, all); }

void Flex::setPadding(float vertical, float horizontal) { setPadding(vertical, horizontal, vertical, horizontal); }

void Flex::setFill(const ColorSpec& color) {
  m_fill = color;
  ensureBackground();
  applyPalette();
}

void Flex::setFill(const Color& color) { setFill(fixedColorSpec(color)); }

void Flex::clearFill() {
  m_fill = clearColorSpec();
  if (m_background != nullptr) {
    applyPalette();
  }
}

void Flex::setRadius(float radius) {
  ensureBackground();
  auto style = m_background->style();
  style.radius = radius;
  m_background->setStyle(style);
}

void Flex::setRadii(const Radii& radii) {
  ensureBackground();
  auto style = m_background->style();
  style.radius = radii;
  m_background->setStyle(style);
}

void Flex::setBorder(const ColorSpec& color, float width) {
  m_border = color;
  ensureBackground();
  auto style = m_background->style();
  style.borderWidth = width;
  m_background->setStyle(style);
  applyPalette();
}

void Flex::setBorder(const Color& color, float width) { setBorder(fixedColorSpec(color), width); }

void Flex::clearBorder() {
  m_border = clearColorSpec();
  if (m_background != nullptr) {
    auto style = m_background->style();
    style.borderWidth = 0.0F;
    m_background->setStyle(style);
    applyPalette();
  }
}

void Flex::applyPalette() {
  if (m_background == nullptr) {
    return;
  }
  auto style = m_background->style();
  style.fill = resolveColorSpec(m_fill);
  style.border = resolveColorSpec(m_border);
  style.fillMode = FillMode::Solid;
  m_background->setStyle(style);
}

void Flex::setSoftness(float softness) {
  ensureBackground();
  auto style = m_background->style();
  style.softness = softness;
  m_background->setStyle(style);
}

void Flex::setCardStyle(float scale, float fillOpacity, bool showBorder) {
  setFill(colorSpecFromRole(ColorRole::SurfaceVariant, fillOpacity));
  if (showBorder) {
    setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
  } else {
    clearBorder();
  }
  setRadius(Style::scaledRadiusXl(scale));
  setPadding(Style::cardPadding * scale);
}

void Flex::setMinWidth(float minWidth) {
  if (m_minWidth == minWidth) {
    return;
  }
  m_minWidth = minWidth;
  markLayoutDirty();
}

void Flex::setMinHeight(float minHeight) {
  if (m_minHeight == minHeight) {
    return;
  }
  m_minHeight = minHeight;
  markLayoutDirty();
}

void Flex::setMaxWidth(float maxWidth) {
  if (m_maxWidth == maxWidth) {
    return;
  }
  m_maxWidth = maxWidth;
  markLayoutDirty();
}

void Flex::setMaxHeight(float maxHeight) {
  if (m_maxHeight == maxHeight) {
    return;
  }
  m_maxHeight = maxHeight;
  markLayoutDirty();
}

void Flex::setWidthPolicy(FlexSizePolicy policy) {
  if (m_widthPolicy == policy) {
    return;
  }
  m_widthPolicy = policy;
  markLayoutDirty();
}

void Flex::setHeightPolicy(FlexSizePolicy policy) {
  if (m_heightPolicy == policy) {
    return;
  }
  m_heightPolicy = policy;
  markLayoutDirty();
}

void Flex::setFillWidth(bool fill) { setWidthPolicy(fill ? FlexSizePolicy::Fill : FlexSizePolicy::Content); }

void Flex::setFillHeight(bool fill) { setHeightPolicy(fill ? FlexSizePolicy::Fill : FlexSizePolicy::Content); }

void Flex::setRowLayout() {
  setDirection(FlexDirection::Horizontal);
  setGap(Style::spaceXs);
  setAlign(FlexAlign::Center);
  setJustify(FlexJustify::Start);
}

void Flex::setChildGapExcluded(Node* child, bool excluded) {
  if (excluded) {
    m_gapExcludedChildren.insert(child);
  } else {
    m_gapExcludedChildren.erase(child);
  }
  markLayoutDirty();
}

Node* Flex::addChild(std::unique_ptr<Node> child) {
  Node* raw = Node::addChild(std::move(child));
  m_gapExcludedChildren.erase(raw);
  return raw;
}

Node* Flex::insertChildAt(std::size_t index, std::unique_ptr<Node> child) {
  Node* raw = Node::insertChildAt(index, std::move(child));
  m_gapExcludedChildren.erase(raw);
  return raw;
}

std::unique_ptr<Node> Flex::removeChild(Node* child) {
  m_gapExcludedChildren.erase(child);
  return Node::removeChild(child);
}

void Flex::ensureBackground() {
  if (m_background != nullptr) {
    return;
  }
  auto rect = std::make_unique<RectNode>();
  rect->setStyle(
      RoundedRectStyle{
          .fill = rgba(0, 0, 0, 0),
          .border = rgba(0, 0, 0, 0),
          .fillMode = FillMode::Solid,
          .radius = 0.0F,
          .softness = 0.0F,
          .borderWidth = 0.0F,
      }
  );
  m_background = static_cast<RectNode*>(addChild(std::move(rect)));
  m_background->setZIndex(-1);
  m_background->setParticipatesInLayout(false);
  m_background->setFrameSize(width(), height());
  applyPalette();
}

void Flex::setSizeFromLayout(float width, float height) {
  m_sizingFromLayout = true;
  setSize(width, height);
  m_sizingFromLayout = false;
}

LayoutSize Flex::runLayout(Renderer& renderer, const LayoutConstraints& constraints, bool arrangeChildren) {
  const bool horizontal = m_direction == FlexDirection::Horizontal;
  const bool mirrored = m_mirrorInRtl && Style::rtl();
  const bool explicitWidth = width() > 0.0F && (arrangingByLayout() || m_explicitWidth);
  const bool explicitHeight = height() > 0.0F && (arrangingByLayout() || m_explicitHeight);

  const auto constrainFlexWidth = [&](float value) {
    float constrained = value;
    if (constraints.hasMaxWidth) {
      constrained = std::min(constrained, constraints.maxWidth);
    }
    constrained = std::max(constrained, constraints.minWidth);
    constrained = std::max(constrained, m_minWidth);
    if (m_maxWidth > 0.0F) {
      constrained = std::min(constrained, m_maxWidth);
    }
    return constrained;
  };

  const auto constrainFlexHeight = [&](float value) {
    float constrained = value;
    if (constraints.hasMaxHeight) {
      constrained = std::min(constrained, constraints.maxHeight);
    }
    constrained = std::max(constrained, constraints.minHeight);
    constrained = std::max(constrained, m_minHeight);
    if (m_maxHeight > 0.0F) {
      constrained = std::min(constrained, m_maxHeight);
    }
    return constrained;
  };

  bool widthKnown = constraints.hasExactWidth();
  bool heightKnown = constraints.hasExactHeight();
  float targetWidth = widthKnown ? constraints.maxWidth : 0.0F;
  float targetHeight = heightKnown ? constraints.maxHeight : 0.0F;

  if (!widthKnown && m_widthPolicy == FlexSizePolicy::Fill && constraints.hasMaxWidth) {
    widthKnown = true;
    targetWidth = constraints.maxWidth;
  }
  if (!heightKnown && m_heightPolicy == FlexSizePolicy::Fill && constraints.hasMaxHeight) {
    heightKnown = true;
    targetHeight = constraints.maxHeight;
  }
  if (!widthKnown && explicitWidth) {
    widthKnown = true;
    targetWidth = width();
  }
  if (!heightKnown && explicitHeight) {
    heightKnown = true;
    targetHeight = height();
  }

  if (widthKnown) {
    targetWidth = constrainFlexWidth(targetWidth);
  }
  if (heightKnown) {
    targetHeight = constrainFlexHeight(targetHeight);
  }

  const bool mainKnown = horizontal ? widthKnown : heightKnown;
  const bool crossKnown = horizontal ? heightKnown : widthKnown;
  const float containerMain = horizontal ? targetWidth : targetHeight;
  const float containerCross = horizontal ? targetHeight : targetWidth;
  const float innerCross = crossKnown
      ? std::max(0.0F, containerCross - crossPaddingStart(*this, horizontal) - crossPaddingEnd(*this, horizontal))
      : 0.0F;

  FlexScratchGuard scratch;
  auto& items = scratch.items();
  items.reserve(children().size());
  float totalGrow = 0.0F;
  for (auto& child : children()) {
    if (!child->visible() || !child->participatesInLayout() || child.get() == m_background) {
      continue;
    }
    auto& item = items.emplace_back();
    item.node = child.get();
    item.gapExcluded = m_gapExcludedChildren.contains(child.get());
    if (child->flexGrow() > 0.0F) {
      totalGrow += child->flexGrow();
    }
  }

  // Wrapping needs a known main extent to break lines against; flexGrow distribution
  // is bypassed so wrapped items keep their natural main size.
  const bool wrapping = m_wrap && mainKnown && !items.empty();

  auto measureItem = [&](ChildLayout& item, bool exactMain, float assignedMain) {
    LayoutConstraints childConstraints;
    if (exactMain) {
      setMainConstraint(childConstraints, horizontal, assignedMain);
    }
    if (crossKnown) {
      if (m_align == FlexAlign::Stretch) {
        setCrossConstraint(childConstraints, horizontal, innerCross);
      } else {
        // Even without stretch, advertise the available cross extent as an upper bound so
        // children that need it (text wrapping, max-width caps) get a budget for free.
        setCrossMaxConstraint(childConstraints, horizontal, innerCross);
      }
    }
    item.measured = item.node->measure(renderer, childConstraints);
    item.main = mainSize(item.measured, horizontal);
    item.cross = (m_align == FlexAlign::Stretch && crossKnown) ? innerCross : crossSize(item.measured, horizontal);
  };

  for (auto& item : items) {
    if (!wrapping && mainKnown && totalGrow > 0.0F && item.node->flexGrow() > 0.0F) {
      continue;
    }
    measureItem(item, false, 0.0F);
  }

  int numGaps = 0;
  {
    bool prevExcluded = items.empty() || items[0].gapExcluded;
    for (size_t i = 1; i < items.size(); ++i) {
      if (!prevExcluded && !items[i].gapExcluded) {
        numGaps++;
      }
      prevExcluded = items[i].gapExcluded;
    }
  }
  const float totalGap = m_gap * static_cast<float>(numGaps);

  if (!wrapping && mainKnown && totalGrow > 0.0F) {
    float fixedMain = mainPaddingStart(*this, horizontal) + mainPaddingEnd(*this, horizontal) + totalGap;
    for (const auto& item : items) {
      if (item.node->flexGrow() <= 0.0F) {
        fixedMain += item.main;
      }
    }
    const float remaining = std::max(0.0F, containerMain - fixedMain);
    for (auto& item : items) {
      if (item.node->flexGrow() <= 0.0F) {
        continue;
      }
      const float share = remaining * (item.node->flexGrow() / totalGrow);
      measureItem(item, true, share);
    }
  }

  if (wrapping) {
    const float padMainStart = mainPaddingStart(*this, horizontal);
    const float padMainEnd = mainPaddingEnd(*this, horizontal);
    const float padCrossStart = crossPaddingStart(*this, horizontal);
    const float innerMain = std::max(0.0F, containerMain - padMainStart - padMainEnd);

    // Greedy line breaking on the main axis: lineBreaks[k] is the index of the
    // first item on line k; lineCrossExtent[k] is that line's max cross size.
    std::vector<std::size_t> lineBreaks;
    std::vector<float> lineCrossExtent;
    lineBreaks.push_back(0);
    float lineMainUsed = 0.0F;
    float lineCross = 0.0F;
    bool lineHasItem = false;
    bool prevExcluded = true;
    for (std::size_t i = 0; i < items.size(); ++i) {
      const float gapBefore = (lineHasItem && !prevExcluded && !items[i].gapExcluded) ? m_gap : 0.0F;
      if (lineHasItem && lineMainUsed + gapBefore + items[i].main > innerMain + 0.5F) {
        lineCrossExtent.push_back(lineCross);
        lineBreaks.push_back(i);
        lineMainUsed = items[i].main;
        lineCross = items[i].cross;
      } else {
        lineMainUsed += gapBefore + items[i].main;
        lineCross = std::max(lineCross, items[i].cross);
      }
      lineHasItem = true;
      prevExcluded = items[i].gapExcluded;
    }
    lineCrossExtent.push_back(lineCross);

    const std::size_t numLines = lineBreaks.size();
    float totalCross = 0.0F;
    for (const float c : lineCrossExtent) {
      totalCross += c;
    }
    if (numLines > 1) {
      totalCross += m_gap * static_cast<float>(numLines - 1);
    }

    const float contentMain = padMainStart + innerMain + padMainEnd;
    const float contentCross = padCrossStart + totalCross + crossPaddingEnd(*this, horizontal);
    LayoutSize desired = sizeFromAxes(horizontal, contentMain, contentCross);
    if (widthKnown) {
      desired.width = targetWidth;
    }
    if (heightKnown) {
      desired.height = targetHeight;
    }
    desired.width = constrainFlexWidth(desired.width);
    desired.height = constrainFlexHeight(desired.height);
    setSizeFromLayout(desired.width, desired.height);

    if (arrangeChildren) {
      float crossCursor = padCrossStart;
      for (std::size_t line = 0; line < numLines; ++line) {
        const std::size_t begin = lineBreaks[line];
        const std::size_t end = (line + 1 < numLines) ? lineBreaks[line + 1] : items.size();
        const float bandCross = lineCrossExtent[line];

        float lineItemsMain = 0.0F;
        int lineGaps = 0;
        prevExcluded = items[begin].gapExcluded;
        for (std::size_t i = begin; i < end; ++i) {
          lineItemsMain += items[i].main;
          if (i > begin && !prevExcluded && !items[i].gapExcluded) {
            lineGaps++;
          }
          prevExcluded = items[i].gapExcluded;
        }

        float effectiveGap = m_gap;
        if (m_justify == FlexJustify::SpaceBetween && (end - begin) > 1 && lineGaps > 0) {
          effectiveGap = std::max(m_gap, (innerMain - lineItemsMain) / static_cast<float>(lineGaps));
        }
        const float lineContentMain = lineItemsMain + effectiveGap * static_cast<float>(lineGaps);

        float cursor = padMainStart;
        if (m_justify == FlexJustify::Center) {
          cursor += std::floor(std::max(0.0F, (innerMain - lineContentMain) * 0.5F));
        } else if (m_justify == FlexJustify::End) {
          cursor += std::max(0.0F, innerMain - lineContentMain);
        }

        bool first = true;
        prevExcluded = items[begin].gapExcluded;
        for (std::size_t i = begin; i < end; ++i) {
          if (!first) {
            if (!prevExcluded && !items[i].gapExcluded) {
              cursor += effectiveGap;
            }
            prevExcluded = items[i].gapExcluded;
          }
          first = false;

          float childCross = items[i].cross;
          float crossPos = crossCursor;
          if (m_align == FlexAlign::Stretch) {
            childCross = bandCross;
          } else {
            const float extraCross = bandCross - childCross;
            if (m_align == FlexAlign::Center) {
              crossPos += std::floor(extraCross * 0.5F);
            } else if (m_align == FlexAlign::End) {
              crossPos += extraCross;
            }
          }

          const float mainPos = horizontal && mirrored ? width() - cursor - items[i].main : cursor;
          const float crossPosPhysical = !horizontal && mirrored ? width() - crossPos - childCross : crossPos;
          items[i].node->arrange(
              renderer,
              rectFromAxes(horizontal, std::round(mainPos), std::round(crossPosPhysical), items[i].main, childCross)
          );
          cursor += items[i].main;
        }
        crossCursor += bandCross + m_gap;
      }
    }
  } else {
    float crossMax = 0.0F;
    float childrenMain = 0.0F;
    for (const auto& item : items) {
      childrenMain += item.main;
      crossMax = std::max(crossMax, item.cross);
    }
    const float contentMain =
        mainPaddingStart(*this, horizontal) + childrenMain + totalGap + mainPaddingEnd(*this, horizontal);
    const float contentCross = crossPaddingStart(*this, horizontal) + crossMax + crossPaddingEnd(*this, horizontal);

    LayoutSize desired = sizeFromAxes(horizontal, contentMain, contentCross);
    if (widthKnown) {
      desired.width = targetWidth;
    }
    if (heightKnown) {
      desired.height = targetHeight;
    }
    desired.width = constrainFlexWidth(desired.width);
    desired.height = constrainFlexHeight(desired.height);
    setSizeFromLayout(desired.width, desired.height);

    if (arrangeChildren) {
      const float finalMain = horizontal ? width() : height();
      const float finalCross = horizontal ? height() : width();
      const float innerMain =
          std::max(0.0F, finalMain - mainPaddingStart(*this, horizontal) - mainPaddingEnd(*this, horizontal));
      const float finalInnerCross =
          std::max(0.0F, finalCross - crossPaddingStart(*this, horizontal) - crossPaddingEnd(*this, horizontal));
      float arrangedChildrenMain = 0.0F;
      for (const auto& item : items) {
        arrangedChildrenMain += item.main;
      }

      float effectiveGap = m_gap;
      if (m_justify == FlexJustify::SpaceBetween && items.size() > 1 && numGaps > 0) {
        effectiveGap = std::max(m_gap, (innerMain - arrangedChildrenMain) / static_cast<float>(numGaps));
      }
      const float arrangedContentMain = arrangedChildrenMain + effectiveGap * static_cast<float>(numGaps);

      float cursor = mainPaddingStart(*this, horizontal);
      if (m_justify == FlexJustify::Center) {
        cursor += std::floor(std::max(0.0F, (innerMain - arrangedContentMain) * 0.5F));
      } else if (m_justify == FlexJustify::End) {
        cursor += std::max(0.0F, innerMain - arrangedContentMain);
      }

      bool first = true;
      bool prevExcluded = items.empty() || items.front().gapExcluded;
      for (auto& item : items) {
        if (!first) {
          if (!prevExcluded && !item.gapExcluded) {
            cursor += effectiveGap;
          }
          prevExcluded = item.gapExcluded;
        }
        first = false;

        float childCross = item.cross;
        float crossPos = crossPaddingStart(*this, horizontal);
        if (m_align == FlexAlign::Stretch) {
          childCross = finalInnerCross;
        } else {
          const float extraCross = finalInnerCross - childCross;
          if (m_align == FlexAlign::Center) {
            crossPos += std::floor(extraCross * 0.5F);
          } else if (m_align == FlexAlign::End) {
            crossPos += extraCross;
          }
        }

        const float mainPos = horizontal && mirrored ? width() - cursor - item.main : cursor;
        const float crossPosPhysical = !horizontal && mirrored ? width() - crossPos - childCross : crossPos;
        item.node->arrange(
            renderer, rectFromAxes(horizontal, std::round(mainPos), std::round(crossPosPhysical), item.main, childCross)
        );
        cursor += item.main;
      }
    }
  }

  if (m_background != nullptr) {
    m_background->setPosition(0.0F, 0.0F);
    m_background->setSize(width(), height());
  }

  return LayoutSize{.width = width(), .height = height()};
}

void Flex::doLayout(Renderer& renderer) {
  LayoutConstraints constraints;
  if ((arrangingByLayout() || m_explicitWidth) && width() > 0.0F) {
    constraints.setExactWidth(width());
  }
  if ((arrangingByLayout() || m_explicitHeight) && height() > 0.0F) {
    constraints.setExactHeight(height());
  }
  runLayout(renderer, constraints, true);
}

LayoutSize Flex::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return runLayout(renderer, constraints, false);
}

void Flex::doArrange(Renderer& renderer, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  runLayout(renderer, LayoutConstraints::exact(rect.width, rect.height), true);
}

LayoutSize Flex::measureByLayout(Renderer& renderer, const LayoutConstraints& constraints) {
  const float measureWidth = constraints.hasExactWidth() ? constraints.maxWidth : (m_explicitWidth ? width() : 0.0F);
  const float measureHeight =
      constraints.hasExactHeight() ? constraints.maxHeight : (m_explicitHeight ? height() : 0.0F);
  setSizeFromLayout(measureWidth, measureHeight);
  doLayout(renderer);
  return constraints.constrain(LayoutSize{.width = width(), .height = height()});
}

void Flex::arrangeByLayout(Renderer& renderer, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  setSizeFromLayout(rect.width, rect.height);
  doLayout(renderer);
}
