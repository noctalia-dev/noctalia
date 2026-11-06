#include "ui/controls/fill_label.h"

#include "core/ui_phase.h"
#include "render/core/renderer.h"

#include <algorithm>
#include <cmath>
#include <glib.h>
#include <limits>
#include <memory>
#include <utility>

FillLabel::FillLabel() {
  setHitTestVisible(false);
  auto label = std::make_unique<Label>();
  label->setMaxLines(1);
  m_label = static_cast<Label*>(addChild(std::move(label)));
}

void FillLabel::setFillRanges(std::vector<FillRange> ranges) {
  if (m_ranges == ranges) {
    return;
  }
  m_ranges = std::move(ranges);
  m_completedSegments = std::min(m_completedSegments, m_ranges.size());
  m_shapeKey.reset();
  markLayoutDirty();
}

void FillLabel::setFillColor(const ColorSpec& color) {
  m_fillColor = color;
  for (auto& clip : m_clips) {
    clip.label->setColor(color);
  }
}

void FillLabel::setFillProgress(std::size_t completedSegments, float fraction) {
  const auto completed = std::min(completedSegments, m_ranges.size());
  const float next = std::isfinite(fraction) ? std::clamp(fraction, 0.0F, 1.0F) : 0.0F;
  if (m_completedSegments == completed && m_fraction == next) {
    return;
  }
  m_completedSegments = completed;
  m_fraction = next;
  applyProgress();
}

void FillLabel::measure(Renderer& renderer) {
  m_label->measure(renderer);
  syncGeometry(renderer);
}

void FillLabel::doLayout(Renderer& renderer) { measure(renderer); }

LayoutSize FillLabel::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  const auto size = static_cast<Node&>(*m_label).measure(renderer, constraints);
  syncGeometry(renderer);
  return size;
}

void FillLabel::doArrange(Renderer& renderer, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  static_cast<Node&>(*m_label).arrange(renderer, {.width = rect.width, .height = rect.height});
  syncGeometry(renderer);
}

void FillLabel::syncGeometry(Renderer& renderer) {
  uiAssertNotRendering("FillLabel::syncGeometry");
  setSize(m_label->width(), m_label->height());
  const ShapeKey key{
      .text = m_label->text(),
      .font = m_label->font(),
      .fontSize = m_label->fontSize(),
      .width = width(),
      .height = height(),
      .maxWidth = m_label->textWrapWidth(),
      .textOriginX = m_label->textOriginX(),
      .baseline = m_label->baselineOffset(),
      .renderScale = renderer.renderScale(),
      .fontGeneration = renderer.textMetricsGeneration(),
      .align = m_label->textAlign(),
      .ellipsize = m_label->ellipsize(),
      .baselineMode = m_label->baselineMode(),
  };
  if (m_shapeKey != key) {
    rebuildFills(renderer);
    m_shapeKey = key;
  }
  applyProgress();
}

void FillLabel::rebuildFills(Renderer& renderer) {
  uiAssertNotRendering("FillLabel::rebuildFills");
  for (auto& clip : m_clips) {
    removeChild(clip.clip);
  }
  m_clips.clear();
  const auto& text = m_label->text();
  if (text.empty() || m_ranges.empty() || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return;
  }

  std::vector<std::size_t> offsets;
  for (const char* cursor = text.data(); cursor < text.data() + text.size(); cursor = g_utf8_next_char(cursor)) {
    offsets.push_back(static_cast<std::size_t>(cursor - text.data()));
  }
  offsets.push_back(text.size());
  std::vector<TextCharSpan> spans;
  std::vector<float> carets;
  renderer.measureTextCharSpans(text, m_label->fontSize(), offsets, spans, m_label->font());
  renderer.measureTextCursorStops(text, m_label->fontSize(), offsets, carets, m_label->font());
  if (spans.size() + 1 != offsets.size() || carets.size() != offsets.size()) {
    return;
  }

  const bool paragraphRtl = carets.front() > carets.back();
  const bool leadingEllipsis = (paragraphRtl && m_label->ellipsize() == TextEllipsize::End)
      || (!paragraphRtl && m_label->ellipsize() == TextEllipsize::Start);
  float originX = m_label->textOriginX();
  if (leadingEllipsis) {
    const auto rightmost = std::ranges::max_element(spans, {}, &TextCharSpan::right);
    originX += std::min(0.0F, width() - rightmost->right);
  }

  for (std::size_t segment = 0; segment < m_ranges.size(); ++segment) {
    const auto& range = m_ranges[segment];
    const auto begin = std::ranges::lower_bound(offsets, range.startByte);
    const auto end = std::ranges::lower_bound(offsets, range.endByte);
    if (begin == offsets.end()
        || end == offsets.end()
        || begin >= end
        || *begin != range.startByte
        || *end != range.endByte) {
      continue;
    }
    const auto first = static_cast<std::size_t>(std::distance(offsets.begin(), begin));
    const auto last = static_cast<std::size_t>(std::distance(offsets.begin(), end));
    float left = std::numeric_limits<float>::max();
    float right = std::numeric_limits<float>::lowest();
    for (auto i = first; i < last; ++i) {
      left = std::min(left, spans[i].left);
      right = std::max(right, spans[i].right);
    }
    left = std::clamp(left + originX, 0.0F, width());
    right = std::clamp(right + originX, 0.0F, width());
    if (right <= left) {
      continue;
    }

    auto clip = std::make_unique<Node>();
    clip->setClipChildren(true);
    clip->setHitTestVisible(false);
    auto fill = std::make_unique<Label>();
    fill->setText(text);
    fill->setFontSize(m_label->fontSize());
    fill->setFontFamily(m_label->font().family);
    fill->setFontFeatures(m_label->font().features);
    fill->setFontWeight(m_label->font().weight);
    fill->setColor(m_fillColor);
    fill->setMaxWidth(m_label->textWrapWidth());
    fill->setMaxLines(1);
    fill->setTextAlign(m_label->textAlign());
    fill->setEllipsize(m_label->ellipsize());
    fill->setBaselineMode(m_label->baselineMode());
    static_cast<Node&>(*fill).arrange(renderer, {.width = width(), .height = height()});
    auto* labelPtr = static_cast<Label*>(clip->addChild(std::move(fill)));
    auto* clipPtr = addChild(std::move(clip));
    const bool rtl = last > first + 1 ? spans[first].left > spans[last - 1].left : carets[first] > carets[last];
    m_clips.push_back(
        {.clip = clipPtr, .label = labelPtr, .segment = segment, .left = left, .right = right, .rtl = rtl}
    );
  }
}

void FillLabel::applyProgress() {
  for (auto& clip : m_clips) {
    const float progress = clip.segment < m_completedSegments ? 1.0F
        : clip.segment == m_completedSegments                 ? m_fraction
                                                              : 0.0F;
    const float fillWidth = (clip.right - clip.left) * progress;
    const float left = clip.rtl ? clip.right - fillWidth : clip.left;
    clip.clip->setPosition(left, 0.0F);
    clip.clip->setFrameSize(fillWidth, height());
    clip.clip->setOpacity(fillWidth > 0.0F ? 1.0F : 0.0F);
    clip.label->setPosition(-left, 0.0F);
  }
}
