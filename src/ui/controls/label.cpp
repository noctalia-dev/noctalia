#include "ui/controls/label.h"

#include "core/deferred_call.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  constexpr const char* kMarqueeGap = " ";

} // namespace

Label::Label() : InputArea() {
  auto textNode = std::make_unique<TextNode>();
  m_textNode = static_cast<TextNode*>(addChild(std::move(textNode)));
  m_textNode->setFontSize(Style::fontSizeBody);
  applyPalette();
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
  // Label is an InputArea; pointer hits on glyphs would otherwise stop here and
  // never reach a parent InputArea (e.g. bar clock/media). Hover-only marquee
  // opts back in via syncHoverInteraction().
  setHitTestVisible(false);
}

bool Label::setText(std::string_view text) {
  if (m_plainText == text) {
    return false;
  }
  m_plainText = std::string(text);
  m_textNode->setText(m_plainText);
  m_measureCached = false;
  return true;
}

void Label::setFontSize(float size) {
  if (m_textNode->fontSize() == size) {
    return;
  }
  m_textNode->setFontSize(size);
  m_measureCached = false;
}

void Label::setFontFamily(std::string family) {
  if (m_textNode->fontFamily() == family) {
    return;
  }
  m_textNode->setFontFamily(std::move(family));
  m_measureCached = false;
}

void Label::setColor(const ColorSpec& color) {
  m_color = color;
  applyPalette();
}

void Label::setColor(const Color& color) { setColor(fixedColorSpec(color)); }

void Label::applyPalette() { m_textNode->setColor(resolveColorSpec(m_color)); }

void Label::setMinWidth(float minWidth) {
  if (m_minWidth == minWidth) {
    return;
  }
  m_minWidth = minWidth;
  m_measureCached = false;
}

void Label::setMaxWidth(float maxWidth) {
  if (m_userMaxWidth == maxWidth) {
    return;
  }
  m_userMaxWidth = maxWidth;
  if (!m_autoScroll) {
    m_textNode->setMaxWidth(maxWidth);
  }
  m_measureCached = false;
}

void Label::setMaxLines(int maxLines) {
  if (m_userMaxLines == maxLines) {
    return;
  }
  m_userMaxLines = maxLines;
  if (!m_autoScroll) {
    m_textNode->setMaxLines(maxLines);
  }
  m_measureCached = false;
}

void Label::setFontWeight(FontWeight fontWeight) {
  if (m_textNode->fontWeight() == fontWeight) {
    return;
  }
  m_textNode->setFontWeight(fontWeight);
  m_measureCached = false;
}

const std::string& Label::text() const noexcept { return m_plainText; }

float Label::fontSize() const noexcept { return m_textNode->fontSize(); }

const Color& Label::color() const noexcept { return m_textNode->color(); }

float Label::maxWidth() const noexcept { return m_userMaxWidth; }

FontWeight Label::fontWeight() const noexcept { return m_textNode->fontWeight(); }

TextAlign Label::textAlign() const noexcept { return m_textNode->textAlign(); }

void Label::setTextAlign(TextAlign align) {
  if (m_textNode->textAlign() == align) {
    return;
  }
  m_textNode->setTextAlign(align);
  m_measureCached = false;
}

void Label::setBaselineMode(LabelBaselineMode mode) {
  if (m_baselineMode == mode) {
    return;
  }
  m_baselineMode = mode;
  m_measureCached = false;
}

void Label::setShadow(const Color& color, float offsetX, float offsetY) {
  m_textNode->setShadow(color, offsetX, offsetY);
}

void Label::clearShadow() { m_textNode->clearShadow(); }

void Label::setAutoScroll(bool enabled) {
  if (m_autoScroll == enabled) {
    return;
  }
  m_autoScroll = enabled;
  stopScrollAnimations();
  m_scrollOffset = 0.0f;
  syncTextNodeConstraints();
  m_measureCached = false;
  syncHoverInteraction();
}

void Label::setAutoScrollOnlyWhenHovered(bool enabled) {
  if (m_autoScrollHoverOnly == enabled) {
    return;
  }
  m_autoScrollHoverOnly = enabled;
  syncHoverInteraction();
  restartScrollIfNeeded();
}

void Label::syncHoverInteraction() {
  if (!m_autoScroll || !m_autoScrollHoverOnly) {
    setOnEnter(nullptr);
    setOnLeave(nullptr);
    setHitTestVisible(false);
    return;
  }
  setHitTestVisible(true);
  setOnEnter([this](const PointerData&) { restartScrollIfNeeded(); });
  setOnLeave([this]() { restartScrollIfNeeded(); });
}

void Label::setAutoScrollSpeed(float pixelsPerSecond) {
  const float next = std::max(pixelsPerSecond, 1.0f);
  if (m_scrollSpeedPxPerSec == next) {
    return;
  }
  m_scrollSpeedPxPerSec = next;
  if (!m_autoScroll) {
    return;
  }
  stopScrollAnimations();
  m_scrollOffset = 0.0f;
  applyScrollPosition();
  markPaintDirty();
  startMarqueeLoop();
}

void Label::syncTextNodeConstraints() {
  if (m_autoScroll) {
    m_textNode->setMaxWidth(0.0f);
    m_textNode->setMaxLines(1);
  } else {
    m_textNode->setMaxWidth(m_userMaxWidth);
    m_textNode->setMaxLines(m_userMaxLines);
  }
}

void Label::applyScrollPosition() { m_textNode->setPosition(m_textBaseX - m_scrollOffset, m_baselineOffset); }

void Label::stopMarqueeAnimation() {
  if (animationManager() != nullptr && m_marqueeAnimId != 0) {
    animationManager()->cancel(m_marqueeAnimId);
  }
  m_marqueeAnimId = 0;
}

void Label::stopSnapAnimation() {
  if (animationManager() != nullptr && m_snapAnimId != 0) {
    animationManager()->cancel(m_snapAnimId);
  }
  m_snapAnimId = 0;
}

void Label::stopScrollAnimations() {
  stopMarqueeAnimation();
  stopSnapAnimation();
}

void Label::startSnapToZero() {
  stopMarqueeAnimation();
  if (m_scrollOffset <= 0.5f) {
    m_scrollOffset = 0.0f;
    applyScrollPosition();
    markPaintDirty();
    return;
  }
  if (animationManager() == nullptr) {
    m_scrollOffset = 0.0f;
    applyScrollPosition();
    markPaintDirty();
    return;
  }
  if (m_snapAnimId != 0) {
    return;
  }
  const float from = m_scrollOffset;
  const float rewindSpeed = m_scrollSpeedPxPerSec * 8.0f;
  float durationMs = std::max(36.0f, (from / rewindSpeed) * 1000.0f);
  durationMs = std::min(durationMs, 180.0f);
  m_snapAnimId = animationManager()->animate(
      from, 0.0f, durationMs, Easing::EaseOutCubic,
      [this](float v) {
        m_scrollOffset = v;
        applyScrollPosition();
        markPaintDirty();
      },
      [this]() {
        m_snapAnimId = 0;
        m_scrollOffset = 0.0f;
        applyScrollPosition();
        markPaintDirty();
      },
      this
  );
}

void Label::startMarqueeLoop() {
  if (!m_autoScroll || animationManager() == nullptr) {
    return;
  }
  if (m_autoScrollHoverOnly && !hovered()) {
    return;
  }
  const float viewportW = width();
  if (viewportW <= 0.0f || m_fullTextWidth <= viewportW + 0.5f) {
    return;
  }
  if (m_marqueeLoopPeriod <= 0.0f) {
    return;
  }
  if (m_marqueeAnimId != 0) {
    return;
  }
  stopSnapAnimation();

  const float period = m_marqueeLoopPeriod;
  const float durationMs = (period / m_scrollSpeedPxPerSec) * 1000.0f;
  m_marqueeAnimId = animationManager()->animate(
      0.0f, period, durationMs, Easing::Linear,
      [this](float v) {
        m_scrollOffset = v;
        applyScrollPosition();
        markPaintDirty();
      },
      [this]() {
        m_marqueeAnimId = 0;
        m_scrollOffset = 0.0f;
        applyScrollPosition();
        markPaintDirty();
        DeferredCall::callLater([this]() { startMarqueeLoop(); });
      },
      this
  );
}

void Label::restartScrollIfNeeded() {
  const bool overflow = m_autoScroll && width() > 0.0f && m_fullTextWidth > width() + 0.5f;
  const bool runMarquee = overflow && (!m_autoScrollHoverOnly || hovered());

  // Skip the reset path when none of the marquee-relevant inputs have changed
  // since the last call. measureWithConstraints() invokes us at the tail of
  // every cache-missed measure, but cache misses are common (different parent
  // constraints across measure/arrange phases) and we must not snap the scroll
  // offset back to 0 unless the geometry or mode actually changed.
  if (m_marqueeStateValid && m_marqueeStateAutoScroll == m_autoScroll &&
      m_marqueeStateHoverOnly == m_autoScrollHoverOnly && m_marqueeStateHovered == hovered() &&
      m_marqueeStateWidth == width() && m_marqueeStateFullTextWidth == m_fullTextWidth &&
      m_marqueeStateLoopPeriod == m_marqueeLoopPeriod && m_marqueeStateSpeed == m_scrollSpeedPxPerSec) {
    if (runMarquee && m_marqueeAnimId == 0 && m_snapAnimId == 0) {
      // Edge case: animation manager wasn't ready when we last tried.
      startMarqueeLoop();
    }
    return;
  }

  m_marqueeStateValid = true;
  m_marqueeStateAutoScroll = m_autoScroll;
  m_marqueeStateHoverOnly = m_autoScrollHoverOnly;
  m_marqueeStateHovered = hovered();
  m_marqueeStateWidth = width();
  m_marqueeStateFullTextWidth = m_fullTextWidth;
  m_marqueeStateLoopPeriod = m_marqueeLoopPeriod;
  m_marqueeStateSpeed = m_scrollSpeedPxPerSec;

  stopMarqueeAnimation();

  if (!overflow) {
    stopSnapAnimation();
    m_scrollOffset = 0.0f;
    setClipChildren(false);
    m_textNode->setText(m_plainText);
    applyScrollPosition();
    markPaintDirty();
    return;
  }

  setClipChildren(true);

  if (!runMarquee) {
    if (m_scrollOffset > 0.5f) {
      startSnapToZero();
    } else {
      m_scrollOffset = 0.0f;
      applyScrollPosition();
      markPaintDirty();
    }
    return;
  }

  stopSnapAnimation();
  m_scrollOffset = 0.0f;
  applyScrollPosition();
  markPaintDirty();
  startMarqueeLoop();
}

void Label::doLayout(Renderer& renderer) { measure(renderer); }

LayoutSize Label::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureWithConstraints(renderer, constraints);
}

void Label::doArrange(Renderer& renderer, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  LayoutConstraints constraints;
  constraints.setExactWidth(rect.width);
  // fromArrange=true: do not overwrite the text node's wrap budget here. Arrange's
  // exact width is the label's own (rounded) measured width fed back, not a parent
  // wrap-intent — feeding it to Pango as maxWidth can trigger sub-pixel ellipsis.
  const LayoutSize measured = measureWithConstraints(renderer, constraints, true);
  setSize(rect.width, rect.height > 0.0f ? rect.height : measured.height);
}

void Label::setCaptionStyle() {
  setFontSize(Style::fontSizeCaption);
  setColor(colorSpecFromRole(ColorRole::OnSurface));
}

void Label::measure(Renderer& renderer) {
  LayoutConstraints constraints;
  measureWithConstraints(renderer, constraints);
}

LayoutSize Label::measureWithConstraints(Renderer& renderer, const LayoutConstraints& constraints, bool fromArrange) {
  const float configuredMaxWidth = m_userMaxWidth;
  float measureMaxWidth = configuredMaxWidth;
  if (constraints.hasMaxWidth) {
    measureMaxWidth =
        configuredMaxWidth > 0.0f ? std::min(configuredMaxWidth, constraints.maxWidth) : constraints.maxWidth;
  }
  if (m_autoScroll) {
    measureMaxWidth = 0.0f;
  }
  const int effectiveMaxLines = m_autoScroll ? 1 : m_userMaxLines;
  const bool singleLine =
      m_autoScroll || (effectiveMaxLines == 1) ||
      (effectiveMaxLines == 0 && configuredMaxWidth <= 0.0f && m_plainText.find('\n') == std::string::npos);
  const TextAlign align = m_textNode->textAlign();
  const FontWeight fontWeight = m_textNode->fontWeight();
  const float renderScale = renderer.renderScale();
  const std::uint64_t textMetricsGeneration = renderer.textMetricsGeneration();
  if (m_measureCached && m_cachedText == m_plainText && m_cachedFontSize == m_textNode->fontSize() &&
      m_cachedFontWeight == fontWeight && m_cachedMaxWidth == m_userMaxWidth && m_cachedMaxLines == m_userMaxLines &&
      m_cachedMinWidth == m_minWidth && m_cachedConstraintMinWidth == constraints.minWidth &&
      m_cachedConstraintMaxWidth == constraints.maxWidth && m_cachedHasConstraintMaxWidth == constraints.hasMaxWidth &&
      m_cachedRenderScale == renderScale && m_cachedTextMetricsGeneration == textMetricsGeneration &&
      m_cachedTextAlign == align && m_cachedBaselineMode == m_baselineMode && m_cachedAutoScroll == m_autoScroll) {
    return LayoutSize{.width = width(), .height = height()};
  }

  // On the arrange path the text node already holds the constraint-aware wrap budget
  // from the preceding measure pass. Resetting it here (via syncTextNodeConstraints)
  // would clobber the correct ellipsis width with m_userMaxWidth, which can be wider
  // than the flex-assigned cell (e.g. leftColumnWidth vs. the actual value-cell share).
  // Only update text node constraints on the measure path.
  if (!fromArrange) {
    syncTextNodeConstraints();
    // Override with constraint-aware wrap budget so paint uses the same wrap width
    // that was used to measure metrics. Without this, a Label inside a Flex with
    // stretch-derived width would measure correctly but paint unwrapped.
    // Skipped for auto-scroll: marquee needs the unconstrained text width.
    if (!m_autoScroll) {
      m_textNode->setMaxWidth(measureMaxWidth);
      m_textNode->setMaxLines(effectiveMaxLines);
    }
  }

  auto metrics = renderer.measureText(
      m_plainText, m_textNode->fontSize(), fontWeight, measureMaxWidth, effectiveMaxLines, align,
      m_textNode->fontFamily()
  );
  const float measuredWidth = measureMaxWidth > 0.0f ? std::min(metrics.width, measureMaxWidth) : metrics.width;
  m_fullTextWidth = m_autoScroll ? measuredWidth : 0.0f;
  const bool hasAssignedWidth = constraints.hasExactWidth();
  const float assignedWidth = constraints.maxWidth;

  const float actualHeight = metrics.bottom - metrics.top;
  const float inkHeight = std::max(0.0f, metrics.inkBottom - metrics.inkTop);
  if (singleLine && inkHeight > 0.0f) {
    float height = 0.0f;
    if (m_baselineMode == LabelBaselineMode::InkCentered) {
      height = std::round(std::max(actualHeight, inkHeight));
      m_baselineOffset = std::round(-metrics.inkTop + (height - inkHeight) * 0.5f);
    } else {
      height = std::round(actualHeight);
      m_baselineOffset = std::round(-metrics.top + (height - actualHeight) * 0.5f);
    }
    float finalWidth = 0.0f;
    if (m_autoScroll) {
      float boxW = m_fullTextWidth;
      if (hasAssignedWidth) {
        boxW = assignedWidth;
      } else {
        if (constraints.hasMaxWidth) {
          boxW = std::min(boxW, constraints.maxWidth);
        }
        if (m_userMaxWidth > 0.0f) {
          boxW = std::min(boxW, m_userMaxWidth);
        }
      }
      finalWidth = std::max(boxW, m_minWidth);
    } else {
      finalWidth = hasAssignedWidth ? std::max(assignedWidth, m_minWidth) : std::max(measuredWidth, m_minWidth);
    }
    setSize(std::round(finalWidth), height);
  } else {
    m_baselineOffset = -metrics.top;
    const float height = actualHeight;
    float finalWidth = 0.0f;
    if (m_autoScroll) {
      float boxW = m_fullTextWidth;
      if (hasAssignedWidth) {
        boxW = assignedWidth;
      } else {
        if (constraints.hasMaxWidth) {
          boxW = std::min(boxW, constraints.maxWidth);
        }
        if (m_userMaxWidth > 0.0f) {
          boxW = std::min(boxW, m_userMaxWidth);
        }
      }
      finalWidth = std::max(boxW, m_minWidth);
    } else {
      finalWidth = hasAssignedWidth ? std::max(assignedWidth, m_minWidth) : std::max(measuredWidth, m_minWidth);
    }
    setSize(std::round(finalWidth), std::round(height));
  }
  if (width() < m_minWidth) {
    setSize(std::round(m_minWidth), height());
  }
  const float layoutWidth = width();
  const bool overflow = m_autoScroll && m_fullTextWidth > layoutWidth + 0.5f;
  const float alignWidth = m_autoScroll ? m_fullTextWidth : measuredWidth;
  float textX = 0.0f;
  if (!overflow) {
    if (align == TextAlign::Center) {
      textX = (layoutWidth - alignWidth) * 0.5f;
    } else if (align == TextAlign::End) {
      textX = layoutWidth - alignWidth;
    }
  }
  m_textBaseX = overflow ? 0.0f : textX;
  if (!overflow) {
    m_scrollOffset = 0.0f;
  }

  if (overflow && m_autoScroll) {
    auto gapMetrics =
        renderer.measureText(kMarqueeGap, m_textNode->fontSize(), fontWeight, 0.0f, 1, align, m_textNode->fontFamily());
    m_marqueeLoopPeriod = m_fullTextWidth + gapMetrics.width;
    m_textNode->setText(m_plainText + kMarqueeGap + m_plainText);
  } else {
    m_marqueeLoopPeriod = 0.0f;
    m_textNode->setText(m_plainText);
  }

  applyScrollPosition();

  m_cachedText = m_plainText;
  m_cachedFontSize = m_textNode->fontSize();
  m_cachedFontWeight = fontWeight;
  m_cachedMaxWidth = m_userMaxWidth;
  m_cachedMaxLines = m_userMaxLines;
  m_cachedMinWidth = m_minWidth;
  m_cachedConstraintMinWidth = constraints.minWidth;
  m_cachedConstraintMaxWidth = constraints.maxWidth;
  m_cachedRenderScale = renderScale;
  m_cachedTextMetricsGeneration = textMetricsGeneration;
  m_cachedHasConstraintMaxWidth = constraints.hasMaxWidth;
  m_cachedTextAlign = align;
  m_cachedBaselineMode = m_baselineMode;
  m_cachedAutoScroll = m_autoScroll;
  m_measureCached = true;

  restartScrollIfNeeded();
  return LayoutSize{.width = width(), .height = height()};
}
