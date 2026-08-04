#include "ui/controls/label.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  constexpr Logger kLog("label");

  constexpr const char* kMarqueeGap = " ";

} // namespace

std::optional<LabelBaselineMode> labelBaselineModeFromToken(std::string_view token) {
  if (token == "text") {
    return LabelBaselineMode::Text;
  }
  if (token == "textFixedHeight") {
    return LabelBaselineMode::TextFixedHeight;
  }
  if (token == "inkCentered") {
    return LabelBaselineMode::InkCentered;
  }
  if (token == "pictographic") {
    return LabelBaselineMode::Pictographic;
  }
  return std::nullopt;
}

Label::Label() {
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

void Label::applyPalette() {
  m_textNode->setColor(resolveColorSpec(m_color));
  if (m_shadowColor.has_value()) {
    m_textNode->setShadow(resolveColorSpec(*m_shadowColor), m_shadowOffsetX, m_shadowOffsetY);
  }
}

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

TextEllipsize Label::ellipsize() const noexcept { return m_textNode->ellipsize(); }

void Label::setEllipsize(TextEllipsize ellipsize) {
  if (m_textNode->ellipsize() == ellipsize) {
    return;
  }
  m_textNode->setEllipsize(ellipsize);
  m_measureCached = false;
}

void Label::setUseMarkup(bool markup) {
  if (m_textNode->useMarkup() == markup) {
    return;
  }
  m_textNode->setUseMarkup(markup);
  m_measureCached = false;
}

void Label::setBaselineMode(LabelBaselineMode mode) {
  if (m_baselineMode == mode) {
    return;
  }
  m_baselineMode = mode;
  m_measureCached = false;
}

void Label::setShadow(const ColorSpec& color, float offsetX, float offsetY) {
  m_shadowColor = color;
  m_shadowOffsetX = offsetX;
  m_shadowOffsetY = offsetY;
  applyPalette();
}

void Label::setShadow(const Color& color, float offsetX, float offsetY) {
  setShadow(fixedColorSpec(color), offsetX, offsetY);
}

void Label::clearShadow() {
  m_shadowColor.reset();
  m_textNode->clearShadow();
}

void Label::setAutoScroll(bool enabled) {
  if (m_autoScroll == enabled) {
    return;
  }
  m_autoScroll = enabled;
  stopScrollAnimations();
  m_scrollOffset = 0.0F;
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
    // A tooltip needs hits to reach the label, so keep hit testing on for it.
    setHitTestVisible(hasTooltip());
    return;
  }
  setHitTestVisible(true);
  setOnEnter([this](const PointerData&) { restartScrollIfNeeded(); });
  setOnLeave([this]() { restartScrollIfNeeded(); });
}

void Label::setAutoScrollSpeed(float pixelsPerSecond) {
  const float next = std::max(pixelsPerSecond, 1.0F);
  if (m_scrollSpeedPxPerSec == next) {
    return;
  }
  m_scrollSpeedPxPerSec = next;
  if (!m_autoScroll) {
    return;
  }
  stopScrollAnimations();
  m_scrollOffset = 0.0F;
  applyScrollPosition();
  startMarqueeLoop();
}

void Label::syncTextNodeConstraints() {
  if (m_autoScroll) {
    m_textNode->setMaxWidth(0.0F);
    m_textNode->setMaxLines(1);
  } else {
    m_textNode->setMaxWidth(m_userMaxWidth);
    m_textNode->setMaxLines(m_userMaxLines);
  }
}

void Label::applyScrollPosition() {
  const float targetX = m_textBaseX - m_scrollOffset;
  const float targetY = m_baselineOffset;

  // Text is snapped in the renderer, so keep the raw fractional position but
  // avoid invalidating the surface while its snapped buffer position is unchanged.
  if (m_marqueeLoopPeriod > 0.0F) {
    float originX = 0.0F;
    float originY = 0.0F;
    float xAxisX = 0.0F;
    float xAxisY = 0.0F;
    float yAxisX = 0.0F;
    float yAxisY = 0.0F;
    Node::mapToScene(this, 0.0F, 0.0F, originX, originY);
    Node::mapToScene(this, 1.0F, 0.0F, xAxisX, xAxisY);
    Node::mapToScene(this, 0.0F, 1.0F, yAxisX, yAxisY);

    constexpr float kTransformEpsilon = 0.0001F;
    const bool translationOnly = std::abs((xAxisX - originX) - 1.0F) <= kTransformEpsilon
        && std::abs(xAxisY - originY) <= kTransformEpsilon
        && std::abs(yAxisX - originX) <= kTransformEpsilon
        && std::abs((yAxisY - originY) - 1.0F) <= kTransformEpsilon;
    if (translationOnly) {
      float currentSceneX = 0.0F;
      float currentSceneY = 0.0F;
      float targetSceneX = 0.0F;
      float targetSceneY = 0.0F;
      Node::absolutePosition(m_textNode, currentSceneX, currentSceneY);
      Node::mapToScene(this, targetX, targetY, targetSceneX, targetSceneY);

      const float scale = std::max(1.0F, m_marqueeRenderScale);
      const bool sameBufferPosition = std::round(currentSceneX * scale) == std::round(targetSceneX * scale)
          && std::round(currentSceneY * scale) == std::round(targetSceneY * scale);
      if (sameBufferPosition) {
        return;
      }
    }
  }

  m_textNode->setPosition(targetX, targetY);
}

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
  if (m_scrollOffset <= 0.5F) {
    m_scrollOffset = 0.0F;
    applyScrollPosition();
    return;
  }
  if (animationManager() == nullptr) {
    m_scrollOffset = 0.0F;
    applyScrollPosition();
    return;
  }
  if (m_snapAnimId != 0) {
    return;
  }
  const float from = m_scrollOffset;
  const float rewindSpeed = m_scrollSpeedPxPerSec * 8.0F;
  float durationMs = std::max(36.0F, (from / rewindSpeed) * 1000.0F);
  durationMs = std::min(durationMs, 180.0F);
  m_snapAnimId = animationManager()->animate(
      from, 0.0F, durationMs, Easing::EaseOutCubic,
      [this](float v) {
        m_scrollOffset = v;
        applyScrollPosition();
      },
      [this]() {
        m_snapAnimId = 0;
        m_scrollOffset = 0.0F;
        applyScrollPosition();
      },
      this
  );
  markPaintDirty();
}

void Label::startMarqueeLoop() {
  if (!m_autoScroll || animationManager() == nullptr) {
    return;
  }
  if (m_autoScrollHoverOnly && !hovered()) {
    return;
  }
  const float viewportW = width();
  if (viewportW <= 0.0F || m_fullTextWidth <= viewportW + 0.5F) {
    return;
  }
  if (m_marqueeLoopPeriod <= 0.0F) {
    return;
  }
  if (m_marqueeAnimId != 0) {
    return;
  }
  stopSnapAnimation();

  const float period = m_marqueeLoopPeriod;
  const float durationMs = (period / m_scrollSpeedPxPerSec) * 1000.0F;
  // Marquee scroll is content motion at a fixed px/sec rate, not a UI transition:
  // it must keep scrolling (and at its own speed) regardless of the global motion
  // enable/speed settings, so drive it off real elapsed time.
  m_marqueeAnimId = animationManager()->animateTimer(
      0.0F, period, durationMs, Easing::Linear,
      [this](float v) {
        m_scrollOffset = v;
        applyScrollPosition();
      },
      [this]() {
        m_marqueeAnimId = 0;
        m_scrollOffset = 0.0F;
        applyScrollPosition();
        const std::weak_ptr<void> aliveGuard = m_aliveGuard;
        DeferredCall::callLater([this, aliveGuard]() {
          if (aliveGuard.expired()) {
            return;
          }
          startMarqueeLoop();
        });
      },
      this
  );
  markPaintDirty();
}

void Label::restartScrollIfNeeded() {
  const bool overflow = m_autoScroll && width() > 0.0F && m_fullTextWidth > width() + 0.5F;
  const bool runMarquee = overflow && (!m_autoScrollHoverOnly || hovered());

  // Skip the reset path when none of the marquee-relevant inputs have changed
  // since the last call. measureWithConstraints() invokes us at the tail of
  // every cache-missed measure, but cache misses are common (different parent
  // constraints across measure/arrange phases) and we must not snap the scroll
  // offset back to 0 unless the geometry or mode actually changed.
  if (m_marqueeStateValid
      && m_marqueeStateAutoScroll == m_autoScroll
      && m_marqueeStateHoverOnly == m_autoScrollHoverOnly
      && m_marqueeStateHovered == hovered()
      && m_marqueeStateWidth == width()
      && m_marqueeStateFullTextWidth == m_fullTextWidth
      && m_marqueeStateLoopPeriod == m_marqueeLoopPeriod
      && m_marqueeStateSpeed == m_scrollSpeedPxPerSec) {
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
    m_scrollOffset = 0.0F;
    setClipChildren(false);
    m_textNode->setText(m_plainText);
    applyScrollPosition();
    return;
  }

  setClipChildren(true);

  if (!runMarquee) {
    if (m_scrollOffset > 0.5F) {
      startSnapToZero();
    } else {
      m_scrollOffset = 0.0F;
      applyScrollPosition();
    }
    return;
  }

  stopSnapAnimation();
  m_scrollOffset = 0.0F;
  applyScrollPosition();
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
  setSize(rect.width, rect.height > 0.0F ? rect.height : measured.height);
}

void Label::measure(Renderer& renderer) {
  LayoutConstraints constraints;
  measureWithConstraints(renderer, constraints);
}

LayoutSize Label::measureWithConstraints(Renderer& renderer, const LayoutConstraints& constraints, bool fromArrange) {
  const float configuredMaxWidth = m_userMaxWidth;
  float measureMaxWidth = configuredMaxWidth;
  if (fromArrange) {
    // Measure with the wrap budget paint will use (the text node's budget from
    // the measure pass), never the arrange width: the arrange width is a box
    // size, not a wrap intent. Re-deciding line breaking here with a different
    // budget can flip the line count — and with it the baseline mode — between
    // measure and paint, bouncing the rendered baseline by a pixel per string.
    measureMaxWidth = m_textNode->maxWidth();
  } else if (constraints.hasMaxWidth) {
    measureMaxWidth =
        configuredMaxWidth > 0.0F ? std::min(configuredMaxWidth, constraints.maxWidth) : constraints.maxWidth;
  }
  if (m_autoScroll) {
    measureMaxWidth = 0.0F;
  }
  const int effectiveMaxLines = m_autoScroll ? 1 : m_userMaxLines;
  const TextAlign align = m_textNode->textAlign();
  const FontWeight fontWeight = m_textNode->fontWeight();
  const float renderScale = renderer.renderScale();
  m_marqueeRenderScale = renderScale;
  const std::uint64_t textMetricsGeneration = renderer.textMetricsGeneration();
  if (m_measureCached
      && m_cachedText == m_plainText
      && m_cachedFontSize == m_textNode->fontSize()
      && m_cachedFontWeight == fontWeight
      && m_cachedMaxWidth == m_userMaxWidth
      && m_cachedMaxLines == m_userMaxLines
      && m_cachedMinWidth == m_minWidth
      && m_cachedConstraintMinWidth == constraints.minWidth
      && m_cachedConstraintMaxWidth == constraints.maxWidth
      && m_cachedHasConstraintMaxWidth == constraints.hasMaxWidth
      && m_cachedRenderScale == renderScale
      && m_cachedTextMetricsGeneration == textMetricsGeneration
      && m_cachedTextAlign == align
      && m_cachedBaselineMode == m_baselineMode
      && m_cachedAutoScroll == m_autoScroll) {
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
      m_textNode->fontFamily(), m_textNode->ellipsize(), m_textNode->useMarkup()
  );
  // Line breaking is decided once, on the measure pass. A divergent line count
  // on arrange means the wrap budget drifted between phases — the baseline mode
  // would flip between measure and paint (renders as ±1px vertical jitter), so
  // surface it loudly instead of letting it land as pixel drift.
  if (!fromArrange) {
    m_measuredLineCount = metrics.lineCount;
  } else if (m_measuredLineCount > 0 && metrics.lineCount != m_measuredLineCount) {
    kLog.warn(
        "label '{}': line count changed between measure ({}) and arrange ({}) — wrap budgets diverged", m_plainText,
        m_measuredLineCount, metrics.lineCount
    );
  }
  // Single- vs multi-line is decided by the measured layout, not by the requested
  // width/line budget: a label with no explicit budget wraps freely, so only the
  // measured line count tells us whether to apply single-line cap-band centering
  // or lay out a multi-line block. Auto-scroll always renders a single marquee line.
  const bool singleLine = m_autoScroll || metrics.lineCount <= 1;
  const float measuredWidth = measureMaxWidth > 0.0F ? std::min(metrics.width, measureMaxWidth) : metrics.width;
  m_fullTextWidth = m_autoScroll ? measuredWidth : 0.0F;
  const bool hasAssignedWidth = constraints.hasExactWidth();
  const float assignedWidth = constraints.maxWidth;

  const float actualHeight = metrics.bottom - metrics.top;
  const float inkHeight = std::max(0.0F, metrics.inkBottom - metrics.inkTop);
  // An icon glyph (Nerd Font / PUA) ignores the cap/x metrics text centering
  // relies on, and its ink can be far wider/taller than the advance. Make the
  // box BE the ink so a container that box-centers the label centers the icon on
  // both axes.
  const bool isIconGlyph = singleLine && inkHeight > 0.0F && StringUtils::isSinglePrivateUseGlyph(m_plainText);
  const float inkWidth = std::max(0.0F, metrics.inkRight - metrics.inkLeft);
  if (singleLine && inkHeight > 0.0F) {
    float height = 0.0F;
    if (m_baselineMode == LabelBaselineMode::InkCentered || isIconGlyph) {
      // Explicit ink mode, or an icon glyph: center the glyph's own ink.
      // Unrounded — the renderer snaps the glyph quad to the pixel grid.
      height = std::round(std::max(actualHeight, inkHeight));
      m_baselineOffset = -metrics.inkTop + (height - inkHeight) * 0.5F;
    } else if (m_baselineMode == LabelBaselineMode::TextFixedHeight) {
      const auto fontMetrics = renderer.measureFont(m_textNode->fontSize(), fontWeight);
      height = std::round(fontMetrics.bottom - fontMetrics.top);
      const float capHeight = fontMetrics.capHeight;
      m_baselineOffset = capHeight > 0.0F ? height * 0.5F + capHeight * 0.5F : -fontMetrics.top;
    } else if (m_baselineMode == LabelBaselineMode::Pictographic) {
      // Center the cap-height band measured from the *ink top* rather than the
      // baseline. For pictographic script fonts (e.g. bongocat poses) the ink top
      // is the fixed part of the art while lower ink moves per glyph; anchoring the
      // band there keeps the art vertically put (no bob) and centres it, where
      // cap-band-from-baseline sits it too high. Degrades to cap-band centering for
      // normal text, whose ink top coincides with the cap top. Unrounded baseline —
      // the renderer snaps the glyph quad to the pixel grid.
      height = std::round(actualHeight);
      const float capHeight = renderer.measureFont(m_textNode->fontSize(), fontWeight).capHeight;
      m_baselineOffset = capHeight > 0.0F ? height * 0.5F - (metrics.inkTop + capHeight * 0.5F)
                                          : -metrics.inkTop + (height - inkHeight) * 0.5F;
    } else {
      height = std::round(actualHeight);
      // Center the cap band (baseline → cap-top) in the box, so a container that
      // box-centers this label sits caps/digits dead-centre. capHeight is the
      // measured cap of 'H', a stable per-font property. Unrounded: the renderer
      // snaps the glyph quad to the pixel grid, so rounding here double-rounds.
      const float capHeight = renderer.measureFont(m_textNode->fontSize(), fontWeight).capHeight;
      m_baselineOffset =
          capHeight > 0.0F ? height * 0.5F + capHeight * 0.5F : -metrics.top + (height - actualHeight) * 0.5F;
    }
    float finalWidth = 0.0F;
    if (m_autoScroll) {
      float boxW = m_fullTextWidth;
      if (hasAssignedWidth) {
        boxW = assignedWidth;
      } else {
        if (constraints.hasMaxWidth) {
          boxW = std::min(boxW, constraints.maxWidth);
        }
        if (m_userMaxWidth > 0.0F) {
          boxW = std::min(boxW, m_userMaxWidth);
        }
      }
      finalWidth = std::max(boxW, m_minWidth);
    } else if (isIconGlyph) {
      // Size to the ink so box-centering can center an icon wider than its advance.
      finalWidth = std::max({inkWidth, measuredWidth, m_minWidth});
    } else {
      finalWidth = hasAssignedWidth ? std::max(assignedWidth, m_minWidth) : std::max(measuredWidth, m_minWidth);
    }
    // Ceil, never round: the box width is fed back to us as an exact arrange
    // constraint, so it must never under-report the text it holds — a box a
    // fraction of a pixel narrower than its own text turns into a wrap/ellipsis
    // trigger downstream.
    setSize(std::ceil(finalWidth), height);
  } else {
    m_baselineOffset = -metrics.top;
    const float inkSpan = inkHeight > 0.0F ? (metrics.inkBottom - metrics.inkTop) : actualHeight;
    const float height = std::max(actualHeight, inkSpan);
    float finalWidth = 0.0F;
    if (m_autoScroll) {
      float boxW = m_fullTextWidth;
      if (hasAssignedWidth) {
        boxW = assignedWidth;
      } else {
        if (constraints.hasMaxWidth) {
          boxW = std::min(boxW, constraints.maxWidth);
        }
        if (m_userMaxWidth > 0.0F) {
          boxW = std::min(boxW, m_userMaxWidth);
        }
      }
      finalWidth = std::max(boxW, m_minWidth);
    } else {
      finalWidth = hasAssignedWidth ? std::max(assignedWidth, m_minWidth) : std::max(measuredWidth, m_minWidth);
    }
    setSize(std::ceil(finalWidth), std::round(height));
  }
  if (width() < m_minWidth) {
    setSize(std::ceil(m_minWidth), height());
  }
  const float layoutWidth = width();
  const bool overflow = m_autoScroll && m_fullTextWidth > layoutWidth + 0.5F;
  const float alignWidth = m_autoScroll ? m_fullTextWidth : measuredWidth;
  float textX = 0.0F;
  if (isIconGlyph) {
    // Center the icon's ink (not its advance) within the box.
    textX = (layoutWidth - inkWidth) * 0.5F - metrics.inkLeft;
  } else if (!overflow) {
    if (align == TextAlign::Center) {
      textX = (layoutWidth - alignWidth) * 0.5F;
    } else if (align == TextAlign::End) {
      textX = layoutWidth - alignWidth;
    }
  }
  m_textBaseX = overflow ? 0.0F : textX;
  if (!overflow) {
    m_scrollOffset = 0.0F;
  }

  if (overflow && m_autoScroll) {
    auto gapMetrics =
        renderer.measureText(kMarqueeGap, m_textNode->fontSize(), fontWeight, 0.0F, 1, align, m_textNode->fontFamily());
    m_marqueeLoopPeriod = m_fullTextWidth + gapMetrics.width;
    m_textNode->setText(m_plainText + kMarqueeGap + m_plainText);
  } else {
    m_marqueeLoopPeriod = 0.0F;
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
