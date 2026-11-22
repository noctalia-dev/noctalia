#include "analog_clock_widget.h"

#include "core/ui_phase.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {
  constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0F;
  constexpr float kSecondAlpha = 0.72F;

  Box* addShape(Node& parent, const ColorSpec& color) {
    auto shape = ui::box({.fill = color});
    shape->setHitTestVisible(false);
    auto* result = shape.get();
    parent.addChild(std::move(shape));
    return result;
  }

  Node* addPivot(Node& parent, const ColorSpec& color) {
    auto pivot = ui::node({.participatesInLayout = false, .hitTestVisible = false});
    addShape(*pivot, color);
    auto* result = pivot.get();
    parent.addChild(std::move(pivot));
    return result;
  }

  Box& pivotShape(Node& pivot) { return *static_cast<Box*>(pivot.children().front().get()); }

  void layoutHand(Node& pivot, float center, float width, float length) {
    pivot.setPosition(center, center);
    auto& hand = pivotShape(pivot);
    hand.setSize(width, length);
    hand.setPosition(-width * 0.5F, -length);
    hand.setRadius(width * 0.5F);
  }

  std::chrono::seconds visibleTime(std::chrono::seconds time, bool showSeconds) {
    return showSeconds ? time : std::chrono::floor<std::chrono::minutes>(time);
  }
} // namespace

AnalogClockWidget::AnalogClockWidget() {
  setHitTestVisible(false);
  m_face = addShape(*this, clearColorSpec());
  for (auto*& tick : m_ticks) {
    tick = addPivot(*this, m_color);
  }
  m_hour = addPivot(*this, m_color);
  m_minute = addPivot(*this, m_color);
  m_second = addPivot(*this, scaleAlpha(m_color, kSecondAlpha));
  m_hub = addShape(*this, m_color);
}

bool AnalogClockWidget::setTime(std::chrono::seconds time) {
  uiAssertNotRendering("AnalogClockWidget::setTime");
  if (time < std::chrono::seconds::zero() || time >= std::chrono::hours{24}) {
    throw std::invalid_argument("Clock time must be seconds since midnight in [0, 24h)");
  }
  const auto previous = visibleTime(m_time, m_showSeconds);
  m_time = time;
  if (visibleTime(m_time, m_showSeconds) == previous) {
    return false;
  }
  updateHands();
  return true;
}

void AnalogClockWidget::setColor(const ColorSpec& color) {
  uiAssertNotRendering("AnalogClockWidget::setColor");
  if (m_color == color) {
    return;
  }
  m_color = color;
  m_face->setBorder(m_color, m_face->style().borderWidth);
  for (auto* tick : m_ticks) {
    pivotShape(*tick).setFill(m_color);
  }
  pivotShape(*m_hour).setFill(m_color);
  pivotShape(*m_minute).setFill(m_color);
  pivotShape(*m_second).setFill(scaleAlpha(m_color, kSecondAlpha));
  m_hub->setFill(m_color);
}

void AnalogClockWidget::setShowCircle(bool show) {
  uiAssertNotRendering("AnalogClockWidget::setShowCircle");
  if (m_showCircle == show) {
    return;
  }
  m_showCircle = show;
  m_face->setVisible(show);
  for (auto* tick : m_ticks) {
    tick->setVisible(show);
  }
}

void AnalogClockWidget::setShowSeconds(bool show) {
  uiAssertNotRendering("AnalogClockWidget::setShowSeconds");
  if (m_showSeconds == show) {
    return;
  }
  m_showSeconds = show;
  m_second->setVisible(show);
  updateHands();
}

void AnalogClockWidget::setContentScale(float scale) {
  uiAssertNotRendering("AnalogClockWidget::setContentScale");
  if (!std::isfinite(scale) || scale <= 0.0F) {
    throw std::invalid_argument("Clock content scale must be positive and finite");
  }
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

void AnalogClockWidget::updateHands() {
  const std::chrono::hh_mm_ss time{visibleTime(m_time, m_showSeconds)};
  const float hour = static_cast<float>(time.hours().count() % 12);
  const float minute = static_cast<float>(time.minutes().count());
  const float second = static_cast<float>(time.seconds().count());
  m_hour->setRotation((hour + minute / 60.0F + second / 3600.0F) * 30.0F * kRadiansPerDegree);
  m_minute->setRotation((minute + second / 60.0F) * 6.0F * kRadiansPerDegree);
  m_second->setRotation(second * 6.0F * kRadiansPerDegree);
}

void AnalogClockWidget::doLayout(Renderer& /*renderer*/) {
  UiPhaseScope phase(UiPhase::Layout);
  const float scale = m_contentScale;
  const float size = Style::fontSizeBody * 5.25F * scale;
  const float center = size * 0.5F;
  const float borderWidth = std::max(1.5F, 2.0F * scale);
  const float hourLength = std::max(4.0F, 5.5F * scale);
  const float hourWidth = std::max(1.75F, 2.25F * scale);
  const float padding = std::max(3.0F, 3.5F * scale) + hourWidth * 0.5F;
  const float radius = std::max(1.0F, center - padding - borderWidth * 0.5F);
  m_face->setPosition(center - radius, center - radius);
  m_face->setSize(radius * 2.0F, radius * 2.0F);
  m_face->setBorder(m_color, borderWidth);
  m_face->setRadius(radius);
  for (std::size_t minute = 0; minute < m_ticks.size(); ++minute) {
    const bool hourMark = minute % 5 == 0;
    const float width = hourMark ? hourWidth : std::max(1.0F, 1.25F * scale);
    const float length = hourMark ? hourLength : std::max(2.5F, 3.0F * scale);
    auto& pivot = *m_ticks[minute];
    pivot.setPosition(center, center);
    pivot.setRotation(static_cast<float>(minute) * 6.0F * kRadiansPerDegree);
    auto& tick = pivotShape(pivot);
    tick.setPosition(-width * 0.5F, -radius);
    tick.setSize(width, length);
    tick.setRadius(width * 0.5F);
  }
  layoutHand(*m_hour, center, std::max(2.2F, 2.65F * scale), radius * 0.54F);
  layoutHand(*m_minute, center, std::max(1.75F, 2.0F * scale), radius * 0.90F);
  layoutHand(*m_second, center, std::max(1.0F, 1.25F * scale), radius * 0.84F);
  const float hubSize = std::max(4.0F, 5.0F * scale);
  m_hub->setSize(hubSize, hubSize);
  m_hub->setPosition(center - hubSize * 0.5F, center - hubSize * 0.5F);
  m_hub->setRadius(hubSize * 0.5F);
  setSize(size, size);
}
