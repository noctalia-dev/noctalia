#include "ui/controls/scroll_view.h"

#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <utility>
#include <wayland-client-protocol.h>

namespace {

  constexpr float kDefaultWidth = 260.0F;
  constexpr float kMinFlingVelocity = 0.12F;
  constexpr float kFlingDeceleration = 0.003F;
  constexpr float kMinScrollAnimDurationMs = 50.0F;
  constexpr float kMaxScrollAnimDurationMs = 900.0F;
  constexpr float kScrollAnimMsPerPx = 0.45F;
  constexpr float kFlingAnimMsPerPx = 0.55F;

  float primaryPosition(const InputArea::PointerData& data, ScrollOrientation orientation) {
    return orientation == ScrollOrientation::Horizontal ? data.localX : data.localY;
  }

  std::uint32_t scrollAxis(ScrollOrientation orientation) {
    return orientation == ScrollOrientation::Horizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL
                                                        : WL_POINTER_AXIS_VERTICAL_SCROLL;
  }

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
          .softness = 1.0F,
          .borderWidth = 0,
      }
  );

  auto viewportArea = std::make_unique<InputArea>();
  viewportArea->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT || !scrollable()) {
      return;
    }
    if (data.pressed) {
      stopScrollAnimation();
      m_dragging = true;
      m_dragStartPosition = primaryPosition(data, m_orientation);
      m_dragStartOffset = m_scrollOffset;
      m_lastDragPosition = m_dragStartPosition;
      m_lastDragSampleAt = std::chrono::steady_clock::now();
      m_dragVelocity = 0.0F;
      return;
    }
    if (m_dragging) {
      m_dragging = false;
      startFling();
    }
  });
  viewportArea->setOnLeave([this]() {
    if (!m_dragging) {
      return;
    }
    m_dragging = false;
    startFling();
  });
  viewportArea->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_viewportArea == nullptr || !m_viewportArea->pressed() || !scrollable()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const float dtMs = std::chrono::duration<float, std::milli>(now - m_lastDragSampleAt).count();
    if (dtMs > 0.0F && dtMs < 80.0F) {
      const float position = primaryPosition(data, m_orientation);
      m_dragVelocity = (position - m_lastDragPosition) / dtMs;
    }
    const float position = primaryPosition(data, m_orientation);
    m_lastDragPosition = position;
    m_lastDragSampleAt = now;

    const float delta = position - m_dragStartPosition;
    applyScrollOffsetValue(clampOffset(m_dragStartOffset - delta));
  });
  viewportArea->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!scrollable() || data.axis != scrollAxis(m_orientation)) {
      return false;
    }

    scrollBy(data.scrollDelta(m_scrollWheelStep));
    return true;
  });
  m_viewportArea = static_cast<InputArea*>(addChild(std::move(viewportArea)));

  auto content = std::make_unique<Flex>();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Start);
  m_content = static_cast<Flex*>(m_viewportArea->addChild(std::move(content)));
  m_content->setPaintContained(true);

  auto scrollbar = std::make_unique<Scrollbar>();
  scrollbar->setOnScrollChanged([this](float offset) { setScrollOffset(offset); });
  m_scrollbar = static_cast<Scrollbar*>(addChild(std::move(scrollbar)));

  applyPalette();
}

void ScrollView::setOrientation(ScrollOrientation orientation) {
  if (m_orientation == orientation) {
    return;
  }
  stopScrollAnimation();
  m_orientation = orientation;
  m_scrollOffset = 0.0F;
  m_targetScrollOffset = 0.0F;
  if (m_scrollbar != nullptr) {
    m_scrollbar->setOrientation(orientation);
  }
  markLayoutDirty();
}

void ScrollView::setScrollOffset(float offset) {
  stopScrollAnimation();
  applyScrollOffsetValue(clampOffset(offset));
}

void ScrollView::scrollBy(float delta) {
  if (delta == 0.0F) {
    return;
  }
  const float base = m_scrollAnimId != 0 ? m_targetScrollOffset : m_scrollOffset;
  animateScrollTo(clampOffset(base + delta));
}

void ScrollView::stopScrollAnimation() {
  if (m_scrollAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_scrollAnimId);
    m_scrollAnimId = 0;
  }
  m_targetScrollOffset = m_scrollOffset;
}

void ScrollView::animateScrollTo(float target, float durationMs) {
  const float clampedTarget = clampOffset(target);

  if (animationManager() == nullptr) {
    applyScrollOffsetValue(clampedTarget);
    return;
  }

  stopScrollAnimation();
  m_targetScrollOffset = clampedTarget;

  if (std::abs(clampedTarget - m_scrollOffset) < 0.5F) {
    applyScrollOffsetValue(clampedTarget);
    return;
  }

  const float distance = std::abs(clampedTarget - m_scrollOffset);
  const float resolvedDuration = durationMs > 0.0F
      ? durationMs
      : std::clamp(distance * kScrollAnimMsPerPx, kMinScrollAnimDurationMs, static_cast<float>(Style::animNormal));

  m_scrollAnimId = animationManager()->animate(
      m_scrollOffset, clampedTarget, resolvedDuration, Easing::EaseOutCubic,
      [this](float value) { applyScrollOffsetValue(value); }, [this]() { m_scrollAnimId = 0; }, this
  );
  markPaintDirty();
}

void ScrollView::startFling() {
  if (animationManager() == nullptr) {
    return;
  }

  const float offsetVelocity = -m_dragVelocity;
  if (std::abs(offsetVelocity) < kMinFlingVelocity) {
    return;
  }

  const float flingDistance = (offsetVelocity * std::abs(offsetVelocity)) / (2.0F * kFlingDeceleration);
  const float target = clampOffset(m_scrollOffset + flingDistance);
  if (std::abs(target - m_scrollOffset) < 1.0F) {
    return;
  }

  const float duration =
      std::clamp(std::abs(flingDistance) * kFlingAnimMsPerPx, kMinScrollAnimDurationMs, kMaxScrollAnimDurationMs);
  animateScrollTo(target, duration);
}

void ScrollView::applyScrollOffsetValue(float offset) {
  const float clamped = clampOffset(offset);
  if (std::abs(clamped - m_scrollOffset) < 0.001F) {
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

void ScrollView::setScrollbarVisible(bool visible) {
  if (m_showScrollbar == visible) {
    return;
  }
  m_showScrollbar = visible;
  markLayoutDirty();
}

void ScrollView::setScrollbarInsetV(float inset) {
  if (m_scrollbar != nullptr) {
    m_scrollbar->setTrackInset(inset);
  }
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
  m_backgroundBorderWidth = 0.0F;
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
    setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
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
    m_targetScrollOffset = m_scrollOffset;
  }
  markLayoutDirty();
}

void ScrollView::setOnScrollChanged(std::function<void(float)> callback) { m_onScrollChanged = std::move(callback); }

void ScrollView::setStickToBottom(bool enabled) { m_stickToBottom = enabled; }

void ScrollView::requestScrollToBottom() {
  stopScrollAnimation();
  m_pendingScrollToBottom = true;
  markLayoutDirty();
}

void ScrollView::setViewportPaddingH(float padding) {
  m_viewportPaddingH = padding;
  markLayoutDirty();
}

void ScrollView::setViewportPaddingV(float padding) {
  m_viewportPaddingV = padding;
  markLayoutDirty();
}

float ScrollView::contentViewportWidth(bool reserveScrollbarGutter) const noexcept {
  const float gutter = m_orientation == ScrollOrientation::Vertical && (m_scrollbarShown || reserveScrollbarGutter)
      ? (Style::scrollbarWidth + Style::scrollbarGap)
      : 0.0F;
  return std::max(0.0F, width() - m_viewportPaddingH * 2.0F - gutter);
}

float ScrollView::contentViewportHeight() const noexcept {
  const float gutter = m_orientation == ScrollOrientation::Horizontal && m_scrollbarShown
      ? (Style::scrollbarWidth + Style::scrollbarGap)
      : 0.0F;
  return std::max(0.0F, height() - m_viewportPaddingV * 2.0F - gutter);
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

  const float w = width() > 0.0F ? width() : kDefaultWidth;
  const float viewportX = m_viewportPaddingH;
  const float viewportY = m_viewportPaddingV;
  const float availableW = std::max(0.0F, w - m_viewportPaddingH * 2.0F);

  // Capture before the orientation branches recompute m_maxScrollOffset:
  // stick-to-bottom must compare against the extents of the previous pass.
  const bool wasAtBottom = m_scrollOffset >= m_maxScrollOffset - 1.0F;

  m_content->setPosition(0.0F, 0.0F);

  if (m_orientation == ScrollOrientation::Horizontal) {
    LayoutSize contentSize = m_content->measure(renderer, {});
    m_scrollbarShown = m_showScrollbar && contentSize.width > availableW + 0.5F;
    const float gutter = m_scrollbarShown ? (Style::scrollbarWidth + Style::scrollbarGap) : 0.0F;
    const float contentWidth = std::max(availableW, contentSize.width);

    LayoutConstraints contentConstraints;
    contentConstraints.setExactWidth(contentWidth);
    contentSize = m_content->measure(renderer, contentConstraints);
    m_content->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = contentWidth, .height = contentSize.height});

    const float naturalH = contentSize.height + m_viewportPaddingV * 2.0F + gutter;
    const float h = height() > 0.0F ? height() : naturalH;
    const float viewportH = std::max(0.0F, h - m_viewportPaddingV * 2.0F - gutter);
    m_viewportWidth = availableW;
    m_viewportHeight = viewportH;
    setSize(w, h);

    m_background->setPosition(0.0F, 0.0F);
    m_background->setFrameSize(w, h);
    m_viewportArea->setPosition(viewportX, viewportY);
    m_viewportArea->setFrameSize(availableW, viewportH);

    m_maxScrollOffset = std::max(0.0F, contentWidth - availableW);
    m_scrollbar->setPosition(viewportX, viewportY + viewportH + Style::scrollbarGap);
    m_scrollbar->setVisible(m_showScrollbar);
    m_scrollbar->update(availableW, contentWidth, m_scrollOffset);
  } else {
    LayoutConstraints contentConstraints;
    contentConstraints.setExactWidth(availableW);
    LayoutSize contentSize = m_content->measure(renderer, contentConstraints);
    m_content->arrange(renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = availableW, .height = contentSize.height});

    const float naturalH = contentSize.height + m_viewportPaddingV * 2.0F;
    const float h = height() > 0.0F ? height() : naturalH;
    const float viewportH = std::max(0.0F, h - m_viewportPaddingV * 2.0F);
    m_viewportHeight = viewportH;
    m_viewportWidth = availableW;
    setSize(w, h);

    m_background->setPosition(0.0F, 0.0F);
    m_background->setFrameSize(w, h);
    m_viewportArea->setPosition(viewportX, viewportY);
    m_viewportArea->setFrameSize(availableW, viewportH);

    m_scrollbarShown = m_showScrollbar && m_content->height() > viewportH + 0.5F;
    const float gutter = m_scrollbarShown ? (Style::scrollbarWidth + Style::scrollbarGap) : 0.0F;
    const float contentWidth = std::max(0.0F, availableW - gutter);
    if (std::abs(m_content->width() - contentWidth) >= 0.5F) {
      contentConstraints = {};
      contentConstraints.setExactWidth(contentWidth);
      contentSize = m_content->measure(renderer, contentConstraints);
      m_content->arrange(
          renderer, LayoutRect{.x = 0.0F, .y = 0.0F, .width = contentWidth, .height = contentSize.height}
      );
    }

    const float contentHeight = m_content->height();
    m_maxScrollOffset = std::max(0.0F, contentHeight - viewportH);
    const float scrollbarX = m_viewportPaddingH + m_viewportWidth - Style::scrollbarWidth;
    m_scrollbar->setPosition(scrollbarX, m_viewportPaddingV);
    m_scrollbar->setVisible(m_showScrollbar);
    m_scrollbar->update(viewportH, contentHeight, m_scrollOffset);
  }

  if (m_boundState != nullptr) {
    m_scrollOffset = clampOffset(m_boundState->offset);
    m_boundState->offset = m_scrollOffset;
  } else {
    m_scrollOffset = clampOffset(m_scrollOffset);
  }
  if (m_scrollAnimId != 0) {
    m_targetScrollOffset = clampOffset(m_targetScrollOffset);
  } else {
    m_targetScrollOffset = m_scrollOffset;
  }
  // A pending jump consumes its flag even when the assignment below is
  // skipped, so a request issued while already at the bottom cannot fire
  // again on a later, unrelated pass.
  const bool jumpToBottom = std::exchange(m_pendingScrollToBottom, false);
  if ((jumpToBottom || (m_stickToBottom && wasAtBottom)) && m_scrollOffset < m_maxScrollOffset) {
    m_scrollOffset = m_maxScrollOffset;
    m_targetScrollOffset = m_maxScrollOffset;
    if (m_boundState != nullptr) {
      m_boundState->offset = m_maxScrollOffset;
    }
    if (m_onScrollChanged) {
      m_onScrollChanged(m_scrollOffset);
    }
  }

  applyScrollOffset();
}

LayoutSize ScrollView::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void ScrollView::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void ScrollView::applyScrollOffset() {
  if (m_content != nullptr) {
    if (m_orientation == ScrollOrientation::Horizontal) {
      m_content->setPosition(-m_scrollOffset, 0.0F);
    } else {
      m_content->setPosition(0.0F, -m_scrollOffset);
    }
  }
  if (m_scrollbar != nullptr && m_scrollbarShown) {
    const float viewportExtent = m_orientation == ScrollOrientation::Horizontal ? m_viewportWidth : m_viewportHeight;
    const float contentExtent = m_content != nullptr
        ? (m_orientation == ScrollOrientation::Horizontal ? m_content->width() : m_content->height())
        : 0.0F;
    m_scrollbar->update(viewportExtent, contentExtent, m_scrollOffset);
  }
}

float ScrollView::clampOffset(float offset) const noexcept { return std::clamp(offset, 0.0F, m_maxScrollOffset); }
