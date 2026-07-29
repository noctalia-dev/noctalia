#include "ui/controls/range_slider.h"

#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/clamp.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>

namespace {

  constexpr double kValueEpsilon = 0.0001;

  RoundedRectStyle solidStyle(const Color& fill, float radius) {
    return RoundedRectStyle{
        .fill = fill,
        .border = fill,
        .fillMode = FillMode::Solid,
        .radius = radius,
        .softness = 1.0f,
        .borderWidth = 0.0f,
    };
  }

  Color resolved(ColorRole role, float alpha = 1.0f) { return colorForRole(role, alpha); }

} // namespace

RangeSlider::RangeSlider() {
  auto track = std::make_unique<RectNode>();
  m_track = static_cast<RectNode*>(addChild(std::move(track)));

  auto fill = std::make_unique<RectNode>();
  m_fill = static_cast<RectNode*>(addChild(std::move(fill)));

  auto lowThumb = std::make_unique<RectNode>();
  m_lowThumb = static_cast<RectNode*>(addChild(std::move(lowThumb)));

  auto highThumb = std::make_unique<RectNode>();
  m_highThumb = static_cast<RectNode*>(addChild(std::move(highThumb)));

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnLeave([this]() {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (!m_enabled || data.button != BTN_LEFT) {
      return;
    }
    if (!data.pressed) {
      m_activeThumb = ActiveThumb::None;
      applyVisualState();
      markPaintDirty();
      if (m_onDragEnd) {
        m_onDragEnd();
      }
      return;
    }
    // Pick the thumb nearest the press; ties (equal values) grab whichever side the press lands on.
    const float lowX = thumbCenterX(m_low);
    const float highX = thumbCenterX(m_high);
    if (std::abs(data.localX - lowX) < std::abs(data.localX - highX)) {
      m_activeThumb = ActiveThumb::Low;
    } else if (std::abs(data.localX - lowX) > std::abs(data.localX - highX)) {
      m_activeThumb = ActiveThumb::High;
    } else {
      m_activeThumb = data.localX < lowX ? ActiveThumb::Low : ActiveThumb::High;
    }
    applyVisualState();
    updateFromLocalX(data.localX);
    markPaintDirty();
  });
  area->setOnMotion([this](const InputArea::PointerData& data) {
    if (!m_enabled || m_inputArea == nullptr || !m_inputArea->pressed()) {
      return;
    }
    updateFromLocalX(data.localX);
  });
  area->setFocusable(true);
  area->setOnFocusGain([this]() {
    if (m_activeThumb == ActiveThumb::None) {
      m_activeThumb = ActiveThumb::Low;
    }
    applyVisualState();
    markPaintDirty();
  });
  area->setOnFocusLoss([this]() {
    m_activeThumb = ActiveThumb::None;
    applyVisualState();
    markPaintDirty();
  });
  area->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (!key.pressed || !m_enabled) {
      return;
    }
    const bool adjustHigh = (key.modifiers & KeyMod::Shift) != 0;
    m_activeThumb = adjustHigh ? ActiveThumb::High : ActiveThumb::Low;
    const double step = m_step > 0.0 ? m_step : (m_max - m_min) * 0.05;
    if (step <= 0.0) {
      return;
    }
    auto nudge = [&](double delta) {
      if (adjustHigh) {
        setHighValue(m_high + delta);
      } else {
        setLowValue(m_low + delta);
      }
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    };
    if (KeybindMatcher::matches(KeybindAction::Left, key.sym, key.modifiers)) {
      nudge(-step);
    } else if (KeybindMatcher::matches(KeybindAction::Right, key.sym, key.modifiers)) {
      nudge(step);
    } else if (KeySymbol::isPageDown(key.sym)) {
      nudge(-step * 10.0);
    } else if (KeySymbol::isPageUp(key.sym)) {
      nudge(step * 10.0);
    } else if (KeySymbol::isHome(key.sym)) {
      if (adjustHigh) {
        setHighValue(m_low);
      } else {
        setLowValue(m_min);
      }
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeySymbol::isEnd(key.sym)) {
      if (adjustHigh) {
        setHighValue(m_max);
      } else {
        setLowValue(m_high);
      }
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    }
    applyVisualState();
    markPaintDirty();
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);

  applyVisualState();
}

void RangeSlider::setRange(double minValue, double maxValue) {
  if (maxValue < minValue) {
    std::swap(minValue, maxValue);
  }
  if (m_min == minValue && m_max == maxValue) {
    return;
  }
  m_min = minValue;
  m_max = maxValue;
  m_low = snapped(m_low);
  m_high = std::max(m_low, snapped(m_high));
  updateGeometry();
  markPaintDirty();
}

void RangeSlider::setStep(double step) {
  m_step = std::max(step, 0.0);
  m_low = snapped(m_low);
  m_high = std::max(m_low, snapped(m_high));
  updateGeometry();
  markPaintDirty();
}

void RangeSlider::setLowValue(double value) {
  const double next = std::min(snapped(value), m_high);
  if (std::abs(next - m_low) < kValueEpsilon) {
    return;
  }
  m_low = next;
  updateGeometry();
  markPaintDirty();
  if (m_onLowChanged) {
    m_onLowChanged(m_low);
  }
}

void RangeSlider::setHighValue(double value) {
  const double next = std::max(snapped(value), m_low);
  if (std::abs(next - m_high) < kValueEpsilon) {
    return;
  }
  m_high = next;
  updateGeometry();
  markPaintDirty();
  if (m_onHighChanged) {
    m_onHighChanged(m_high);
  }
}

void RangeSlider::setValues(double low, double high) {
  double lo = snapped(low);
  double hi = snapped(high);
  if (hi < lo) {
    std::swap(lo, hi);
  }
  if (std::abs(lo - m_low) < kValueEpsilon && std::abs(hi - m_high) < kValueEpsilon) {
    return;
  }
  m_low = lo;
  m_high = hi;
  updateGeometry();
  markPaintDirty();
}

void RangeSlider::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  applyVisualState();
  markPaintDirty();
}

void RangeSlider::setTrackHeight(float height) {
  m_trackHeight = std::max(1.0f, height);
  updateGeometry();
  markLayoutDirty();
}

void RangeSlider::setThumbSize(float size) {
  m_thumbSizePx = std::max(1.0f, size);
  updateGeometry();
  markLayoutDirty();
}

void RangeSlider::setControlHeight(float height) {
  m_controlHeightPx = std::max(1.0f, height);
  updateGeometry();
  markLayoutDirty();
}

void RangeSlider::setOnLowChanged(std::function<void(double)> callback) { m_onLowChanged = std::move(callback); }

void RangeSlider::setOnHighChanged(std::function<void(double)> callback) { m_onHighChanged = std::move(callback); }

void RangeSlider::setOnDragEnd(std::function<void()> callback) { m_onDragEnd = std::move(callback); }

bool RangeSlider::dragging() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void RangeSlider::doLayout(Renderer& /*renderer*/) {
  updateGeometry();
  applyVisualState();
}

LayoutSize RangeSlider::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void RangeSlider::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void RangeSlider::updateGeometry() {
  const float widthPx = width() > 0.0f ? width() : Style::sliderDefaultWidth;
  const float heightPx = std::max({m_thumbSizePx, m_trackHeight, m_controlHeightPx});
  setSize(widthPx, heightPx);

  const float trackY = (heightPx - m_trackHeight) * 0.5f;
  const float trackX = Style::sliderHorizontalPadding;
  const float trackW = std::max(0.0f, widthPx - Style::sliderHorizontalPadding * 2.0f);
  const float lowX = trackX + normalized(m_low) * trackW;
  const float highX = trackX + normalized(m_high) * trackW;
  const float thumbY = (heightPx - m_thumbSizePx) * 0.5f;

  m_track->setPosition(trackX, trackY);
  m_track->setFrameSize(trackW, m_trackHeight);

  m_fill->setPosition(lowX, trackY);
  m_fill->setFrameSize(std::max(0.0f, highX - lowX), m_trackHeight);

  const float maxThumbX = trackX + trackW - m_thumbSizePx;
  m_lowThumb->setPosition(util::clampOrdered(lowX - m_thumbSizePx * 0.5f, trackX, maxThumbX), thumbY);
  m_lowThumb->setFrameSize(m_thumbSizePx, m_thumbSizePx);
  m_highThumb->setPosition(util::clampOrdered(highX - m_thumbSizePx * 0.5f, trackX, maxThumbX), thumbY);
  m_highThumb->setFrameSize(m_thumbSizePx, m_thumbSizePx);

  m_inputArea->setPosition(0.0f, 0.0f);
  m_inputArea->setFrameSize(widthPx, heightPx);
}

void RangeSlider::updateFromLocalX(float x) {
  const float widthPx = width() > 0.0f ? width() : Style::sliderDefaultWidth;
  const float trackX = Style::sliderHorizontalPadding;
  const float trackW = std::max(0.0f, widthPx - Style::sliderHorizontalPadding * 2.0f);
  if (trackW <= 0.0f || m_activeThumb == ActiveThumb::None) {
    return;
  }
  const double t = static_cast<double>(std::clamp((x - trackX) / trackW, 0.0f, 1.0f));
  const double value = m_min + t * (m_max - m_min);
  if (m_activeThumb == ActiveThumb::Low) {
    setLowValue(value);
  } else {
    setHighValue(value);
  }
}

void RangeSlider::applyVisualState() {
  const bool hovering = m_inputArea != nullptr && m_inputArea->hovered();
  const bool focused = m_inputArea != nullptr && m_inputArea->focused();

  Color trackColor = resolved(ColorRole::Outline);
  Color fillColor = resolved(ColorRole::Primary);
  Color thumbColor = resolved(ColorRole::OnPrimary);
  Color thumbBorder = resolved(ColorRole::Outline);

  m_lowThumb->setVisible(m_enabled);
  m_highThumb->setVisible(m_enabled);

  if (!m_enabled) {
    trackColor = resolved(ColorRole::Outline, Style::disabledOutlineAlpha);
    fillColor = resolved(ColorRole::Primary, 0.5f);
  } else if (focused) {
    thumbBorder = resolveColorSpec(focusRingColorSpec());
  } else if (hovering) {
    thumbBorder = resolved(ColorRole::Hover);
  }

  m_track->setStyle(solidStyle(trackColor, m_trackHeight * 0.5f));
  m_fill->setStyle(solidStyle(fillColor, m_trackHeight * 0.5f));

  auto thumbStyle = solidStyle(thumbColor, m_thumbSizePx * 0.5f);
  thumbStyle.border = thumbBorder;
  thumbStyle.borderWidth = focused ? Style::focusRingWidth : Style::borderWidth;
  m_lowThumb->setStyle(thumbStyle);
  m_highThumb->setStyle(thumbStyle);

  if (m_enabled && focused && m_activeThumb != ActiveThumb::None) {
    auto activeStyle = thumbStyle;
    activeStyle.border = resolveColorSpec(focusRingColorSpec());
    activeStyle.borderWidth = Style::focusRingWidth;
    if (m_activeThumb == ActiveThumb::Low) {
      m_lowThumb->setStyle(activeStyle);
    } else {
      m_highThumb->setStyle(activeStyle);
    }
  }
}

float RangeSlider::normalized(double value) const noexcept {
  if (m_max <= m_min) {
    return 0.0f;
  }
  return static_cast<float>(std::clamp((value - m_min) / (m_max - m_min), 0.0, 1.0));
}

float RangeSlider::thumbCenterX(double value) const noexcept {
  const float widthPx = width() > 0.0f ? width() : Style::sliderDefaultWidth;
  const float trackX = Style::sliderHorizontalPadding;
  const float trackW = std::max(0.0f, widthPx - Style::sliderHorizontalPadding * 2.0f);
  return trackX + normalized(value) * trackW;
}

double RangeSlider::snapped(double value) const noexcept {
  const double clamped = std::clamp(value, m_min, m_max);
  if (m_step <= 0.0 || m_max <= m_min) {
    return clamped;
  }
  const double steps = std::round((clamped - m_min) / m_step);
  return std::clamp(m_min + steps * m_step, m_min, m_max);
}
