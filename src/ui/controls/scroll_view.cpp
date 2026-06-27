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
#include <wayland-client-protocol.h>

namespace {

  constexpr float kDefaultWidth = 260.0f;
  constexpr float kMinFlingVelocity = 0.12f;
  constexpr float kFlingDeceleration = 0.003f;
  constexpr float kMinScrollAnimDurationMs = 50.0f;
  constexpr float kMaxScrollAnimDurationMs = 900.0f;
  constexpr float kScrollAnimMsPerPx = 0.45f;
  constexpr float kFlingAnimMsPerPx = 0.55f;

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
    if (data.button != BTN_LEFT || !scrollable()) {
      return;
    }
    if (data.pressed) {
      stopScrollAnimation();
      m_dragging = true;
      m_dragStartLocalY = data.localY;
      m_dragStartOffset = m_scrollOffset;
      m_lastDragLocalY = data.localY;
      m_lastDragSampleAt = std::chrono::steady_clock::now();
      m_dragVelocity = 0.0f;
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
    if (dtMs > 0.0f && dtMs < 80.0f) {
      m_dragVelocity = (data.localY - m_lastDragLocalY) / dtMs;
    }
    m_lastDragLocalY = data.localY;
    m_lastDragSampleAt = now;

    const float delta = data.localY - m_dragStartLocalY;
    applyScrollOffsetValue(clampOffset(m_dragStartOffset - delta));
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
  stopScrollAnimation();
  applyScrollOffsetValue(clampOffset(offset));
}

void ScrollView::scrollBy(float delta) {
  if (delta == 0.0f) {
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

  if (std::abs(clampedTarget - m_scrollOffset) < 0.5f) {
    applyScrollOffsetValue(clampedTarget);
    return;
  }

  const float distance = std::abs(clampedTarget - m_scrollOffset);
  const float resolvedDuration = durationMs > 0.0f
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

  const float flingDistance = (offsetVelocity * std::abs(offsetVelocity)) / (2.0f * kFlingDeceleration);
  const float target = clampOffset(m_scrollOffset + flingDistance);
  if (std::abs(target - m_scrollOffset) < 1.0f) {
    return;
  }

  const float duration =
      std::clamp(std::abs(flingDistance) * kFlingAnimMsPerPx, kMinScrollAnimDurationMs, kMaxScrollAnimDurationMs);
  animateScrollTo(target, duration);
}

void ScrollView::applyScrollOffsetValue(float offset) {
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
  if (m_scrollAnimId != 0) {
    m_targetScrollOffset = clampOffset(m_targetScrollOffset);
  } else {
    m_targetScrollOffset = m_scrollOffset;
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
