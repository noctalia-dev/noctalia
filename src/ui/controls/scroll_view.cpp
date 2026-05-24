#include "ui/controls/scroll_view.h"

#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <wayland-client-protocol.h>

namespace {

  constexpr float kDefaultWidth = 260.0f;

} // namespace

ScrollView::ScrollView() {
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
  setClipChildren(true);

  auto background = std::make_unique<RectNode>();
  m_background = static_cast<RectNode*>(addChild(std::move(background)));
  m_background->setStyle(
      RoundedRectStyle{
          .fill = clearColor(),
          .border = clearColor(),
          .fillMode = FillMode::Solid,
          .radius = Style::scaledRadiusMd(),
          .softness = 1.0f,
          .borderWidth = 0,
      }
  );

  auto viewportArea = std::make_unique<InputArea>();
  viewportArea->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT || !data.pressed || !scrollable()) {
      return;
    }
    m_dragStartLocalY = data.localY;
    m_dragStartOffset = m_scrollOffset;
  });
  viewportArea->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_viewportArea == nullptr || !m_viewportArea->pressed() || !scrollable()) {
      return;
    }
    const float delta = data.localY - m_dragStartLocalY;
    setScrollOffset(m_dragStartOffset - delta);
  });
  viewportArea->setOnAxis([this](const InputArea::PointerData& data) {
    if (!scrollable()) {
      return;
    }

    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }

    scrollBy(data.scrollDelta(m_scrollWheelStep));
  });
  m_viewportArea = static_cast<InputArea*>(addChild(std::move(viewportArea)));

  auto content = std::make_unique<Flex>();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Start);
  m_content = static_cast<Flex*>(m_viewportArea->addChild(std::move(content)));

  auto scrollbar = std::make_unique<Scrollbar>();
  scrollbar->setOnScrollChanged([this](float offset) { setScrollOffset(offset); });
  m_scrollbar = static_cast<Scrollbar*>(addChild(std::move(scrollbar)));

  applyPalette();
}

void ScrollView::setScrollOffset(float offset) {
  const float clamped = clampOffset(offset);
  if (std::abs(clamped - m_scrollOffset) < 0.001f) {
    return;
  }
  m_scrollOffset = clamped;
  if (m_boundState != nullptr) {
    m_boundState->offset = m_scrollOffset;
  }
  applyScrollOffset();
  markPaintDirty();
  if (m_onScrollChanged) {
    m_onScrollChanged(m_scrollOffset);
  }
}

void ScrollView::scrollBy(float delta) { setScrollOffset(m_scrollOffset + delta); }

void ScrollView::setScrollbarVisible(bool visible) {
  if (m_showScrollbar == visible) {
    return;
  }
  m_showScrollbar = visible;
  markLayoutDirty();
}

void ScrollView::setFill(const ColorSpec& fill) {
  m_backgroundFill = fill;
  applyPalette();
}

void ScrollView::setFill(const Color& fill) { setFill(fixedColorSpec(fill)); }

void ScrollView::clearFill() {
  m_backgroundFill = clearColorSpec();
  applyPalette();
}

void ScrollView::setBorder(const ColorSpec& border, float width) {
  m_backgroundBorder = border;
  m_backgroundBorderWidth = width;
  applyPalette();
}

void ScrollView::setBorder(const Color& border, float width) { setBorder(fixedColorSpec(border), width); }

void ScrollView::clearBorder() {
  m_backgroundBorder = clearColorSpec();
  m_backgroundBorderWidth = 0.0f;
  applyPalette();
}

void ScrollView::setRadius(float radius) {
  m_backgroundRadius = radius;
  applyPalette();
}

void ScrollView::setSoftness(float softness) {
  m_backgroundSoftness = softness;
  applyPalette();
}

void ScrollView::setCardStyle(float scale, float fillOpacity, bool showBorder) {
  setFill(colorSpecFromRole(ColorRole::SurfaceVariant, fillOpacity));
  if (showBorder) {
    setBorder(colorSpecFromRole(ColorRole::Outline, 0.5f), Style::borderWidth);
  } else {
    clearBorder();
  }
  setRadius(Style::scaledRadiusXl(scale));
  setViewportPaddingH(Style::cardPadding * scale);
  setViewportPaddingV(Style::cardPadding * scale);
}

void ScrollView::bindState(ScrollViewState* state) {
  m_boundState = state;
  if (m_boundState != nullptr) {
    m_scrollOffset = m_boundState->offset;
  }
  markLayoutDirty();
}

void ScrollView::setOnScrollChanged(std::function<void(float)> callback) { m_onScrollChanged = std::move(callback); }

void ScrollView::setViewportPaddingH(float padding) {
  m_viewportPaddingH = padding;
  markLayoutDirty();
}

void ScrollView::setViewportPaddingV(float padding) {
  m_viewportPaddingV = padding;
  markLayoutDirty();
}

float ScrollView::contentViewportWidth() const noexcept {
  const float gutter = m_scrollbarShown ? (Style::scrollbarWidth + Style::scrollbarGap) : 0.0f;
  return std::max(0.0f, width() - m_viewportPaddingH * 2.0f - gutter);
}

float ScrollView::contentViewportHeight() const noexcept {
  return std::max(0.0f, height() - m_viewportPaddingV * 2.0f);
}

void ScrollView::applyPalette() {
  if (m_background != nullptr) {
    m_background->setStyle(
        RoundedRectStyle{
            .fill = resolveColorSpec(m_backgroundFill),
            .border = resolveColorSpec(m_backgroundBorder),
            .fillMode = FillMode::Solid,
            .radius = m_backgroundRadius,
            .softness = m_backgroundSoftness,
            .borderWidth = m_backgroundBorderWidth,
        }
    );
  }
}

void ScrollView::doLayout(Renderer& renderer) {
  if (m_background == nullptr || m_viewportArea == nullptr || m_content == nullptr || m_scrollbar == nullptr) {
    return;
  }

  const float w = width() > 0.0f ? width() : kDefaultWidth;
  const float viewportX = m_viewportPaddingH;
  const float viewportY = m_viewportPaddingV;
  const float viewportW = std::max(0.0f, w - m_viewportPaddingH * 2.0f);

  m_content->setPosition(0.0f, 0.0f);
  LayoutConstraints contentConstraints;
  contentConstraints.setExactWidth(viewportW);
  LayoutSize contentSize = m_content->measure(renderer, contentConstraints);
  m_content->arrange(renderer, LayoutRect{.x = 0.0f, .y = 0.0f, .width = viewportW, .height = contentSize.height});

  const float naturalH = contentSize.height + m_viewportPaddingV * 2.0f;
  const float h = height() > 0.0f ? height() : naturalH;
  const float viewportH = std::max(0.0f, h - m_viewportPaddingV * 2.0f);
  m_viewportHeight = viewportH;
  m_viewportWidth = viewportW;
  setSize(w, h);

  m_background->setPosition(0.0f, 0.0f);
  m_background->setFrameSize(w, h);
  m_viewportArea->setPosition(viewportX, viewportY);
  m_viewportArea->setFrameSize(viewportW, viewportH);

  m_scrollbarShown = m_showScrollbar && m_content->height() > viewportH + 0.5f;
  const float gutter = m_scrollbarShown ? (Style::scrollbarWidth + Style::scrollbarGap) : 0.0f;
  const float contentWidth = std::max(0.0f, viewportW - gutter);
  if (std::abs(m_content->width() - contentWidth) >= 0.5f) {
    contentConstraints = {};
    contentConstraints.setExactWidth(contentWidth);
    contentSize = m_content->measure(renderer, contentConstraints);
    m_content->arrange(renderer, LayoutRect{.x = 0.0f, .y = 0.0f, .width = contentWidth, .height = contentSize.height});
  }

  const float contentHeight = m_content->height();
  m_maxScrollOffset = std::max(0.0f, contentHeight - viewportH);
  if (m_boundState != nullptr) {
    m_scrollOffset = clampOffset(m_boundState->offset);
    m_boundState->offset = m_scrollOffset;
  } else {
    m_scrollOffset = clampOffset(m_scrollOffset);
  }

  const float scrollbarX = m_viewportPaddingH + m_viewportWidth - Style::scrollbarWidth;
  m_scrollbar->setPosition(scrollbarX, m_viewportPaddingV);
  m_scrollbar->setVisible(m_showScrollbar);
  m_scrollbar->update(viewportH, contentHeight, m_scrollOffset);

  applyScrollOffset();
}

LayoutSize ScrollView::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void ScrollView::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void ScrollView::applyScrollOffset() {
  if (m_content != nullptr) {
    m_content->setPosition(0.0f, -m_scrollOffset);
  }
  if (m_scrollbar != nullptr && m_scrollbarShown) {
    m_scrollbar->update(m_viewportHeight, m_content != nullptr ? m_content->height() : 0.0f, m_scrollOffset);
  }
}

float ScrollView::clampOffset(float offset) const noexcept { return std::clamp(offset, 0.0f, m_maxScrollOffset); }
