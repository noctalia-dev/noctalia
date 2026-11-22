#include "clock_widget.h"

#include "core/ui_phase.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {
  constexpr float kFontScale = 4.0F;
  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;
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

DesktopClock::DesktopClock() {
  setHitTestVisible(false);
  addChild(
      ui::label({
          .out = &m_label,
          .fontSize = ::Style::fontSizeBody * kFontScale,
          .fontWeight = FontWeight::Bold,
          .fontFeatures = "tnum",
          .maxLines = 0,
          .textAlign = TextAlign::Center,
      })
  );
  applyShadow();
  auto analogRoot = ui::node({.visible = false, .hitTestVisible = false});
  m_analogRoot = analogRoot.get();
  addChild(std::move(analogRoot));
  m_face = addShape(*m_analogRoot, clearColorSpec());
  for (auto*& tick : m_ticks) {
    tick = addPivot(*m_analogRoot, m_color);
  }
  m_hour = addPivot(*m_analogRoot, m_color);
  m_minute = addPivot(*m_analogRoot, m_color);
  m_second = addPivot(*m_analogRoot, scaleAlpha(m_color, kSecondAlpha));
  m_hub = addShape(*m_analogRoot, m_color);
}

bool DesktopClock::setText(std::string_view text) {
  uiAssertNotRendering("DesktopClock::setText");
  return m_label->setText(text);
}

void DesktopClock::setFontFamily(std::string family) {
  uiAssertNotRendering("DesktopClock::setFontFamily");
  if (m_label->font().family == family) {
    return;
  }
  m_label->setFontFamily(std::move(family));
  m_metricsDirty = true;
}

void DesktopClock::setFontSize(float size) {
  uiAssertNotRendering("DesktopClock::setFontSize");
  if (!std::isfinite(size) || size < 0) {
    throw std::invalid_argument("Clock font size must be nonnegative and finite");
  }
  if (m_fontSize == size) {
    return;
  }
  m_fontSize = size;
  m_metricsDirty = true;
  markLayoutDirty();
}
void DesktopClock::setFontWeight(FontWeight weight) {
  uiAssertNotRendering("DesktopClock::setFontWeight");
  if (m_label->font().weight == weight) {
    return;
  }
  m_label->setFontWeight(weight);
  m_metricsDirty = true;
  markLayoutDirty();
}
void DesktopClock::setTightText(bool enabled) {
  uiAssertNotRendering("DesktopClock::setTightText");
  m_label->setTightInkBounds(enabled);
  m_label->setMaxLines(enabled ? 1 : 0);
  markLayoutDirty();
}

void DesktopClock::setColor(const ColorSpec& color) {
  uiAssertNotRendering("DesktopClock::setColor");
  if (m_color == color) {
    return;
  }
  m_color = color;
  m_label->setColor(color);
  m_face->setBorder(m_color, m_face->style().borderWidth);
  for (auto* tick : m_ticks) {
    pivotShape(*tick).setFill(m_color);
  }
  pivotShape(*m_hour).setFill(m_color);
  pivotShape(*m_minute).setFill(m_color);
  pivotShape(*m_second).setFill(scaleAlpha(m_color, kSecondAlpha));
  m_hub->setFill(m_color);
}

void DesktopClock::setShadow(bool enabled) {
  uiAssertNotRendering("DesktopClock::setShadow");
  if (m_shadow == enabled) {
    return;
  }
  m_shadow = enabled;
  applyShadow();
  markLayoutDirty();
}

void DesktopClock::setCenterText(bool center) {
  uiAssertNotRendering("DesktopClock::setCenterText");
  if (m_centerText == center) {
    return;
  }
  m_centerText = center;
  m_label->setTextAlign(center ? TextAlign::Center : TextAlign::Start);
}

void DesktopClock::setContentScale(float scale) {
  uiAssertNotRendering("DesktopClock::setContentScale");
  if (!std::isfinite(scale) || scale <= 0.0F) {
    throw std::invalid_argument("Clock content scale must be positive and finite");
  }
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  m_metricsDirty = true;
  markLayoutDirty();
}

const std::string& DesktopClock::text() const noexcept { return m_label->text(); }
const std::string& DesktopClock::fontFamily() const noexcept { return m_label->font().family; }

void DesktopClock::updateStableWidth(Renderer& renderer, float fontSize) {
  const bool metricsChanged = m_metricsDirty
      || m_metricsRenderScale != renderer.renderScale()
      || m_metricsGeneration != renderer.textMetricsGeneration();
  const auto& font = m_label->font();
  if (metricsChanged) {
    m_metricsDirty = false;
    m_metricsRenderScale = renderer.renderScale();
    m_metricsGeneration = renderer.textMetricsGeneration();
    float widest = -1.0F;
    for (char digit = '0'; digit <= '9'; ++digit) {
      const float width = renderer.measureText(std::string(1, digit), fontSize, font).width;
      if (width > widest) {
        widest = width;
        m_widestDigit = digit;
      }
    }
  }
  std::string sample = m_label->text();
  for (char& ch : sample) {
    if (ch >= '0' && ch <= '9') {
      ch = m_widestDigit;
    }
  }
  if (metricsChanged || sample != m_sample) {
    m_sample = std::move(sample);
    m_stableWidth = renderer.measureText(m_sample, fontSize, font).width;
  }
}

void DesktopClock::applyShadow() {
  if (m_shadow) {
    const float offset = kShadowOffset * m_contentScale;
    m_label->setShadow(colorSpecFromRole(ColorRole::Shadow, kShadowAlpha), offset, offset);
  } else {
    m_label->clearShadow();
  }
}

void DesktopClock::doLayout(Renderer& renderer) {
  UiPhaseScope phase(UiPhase::Layout);
  if (m_style == Style::Analog) {
    layoutAnalog();
  } else {
    layoutDigital(renderer);
  }
}

void DesktopClock::layoutDigital(Renderer& renderer) {
  m_label->setVisible(!m_label->text().empty());
  if (m_label->text().empty()) {
    setSize(0, 0);
    return;
  }
  const float fontSize = (m_fontSize > 0 ? m_fontSize : ::Style::fontSizeBody * kFontScale) * m_contentScale;
  updateStableWidth(renderer, fontSize);
  m_label->setFontSize(fontSize);
  m_label->setMinWidth(m_stableWidth);
  m_label->setMaxWidth(m_stableWidth);
  m_label->measure(renderer);
  m_label->setPosition(0.0F, 0.0F);
  const float shadowPad = m_shadow ? kShadowOffset * m_contentScale : 0.0F;
  setSize(m_label->width() + shadowPad, m_label->height() + shadowPad);
  applyShadow();
}

bool DesktopClock::setTime(std::chrono::seconds time) {
  uiAssertNotRendering("DesktopClock::setTime");
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

void DesktopClock::setShowCircle(bool show) {
  uiAssertNotRendering("DesktopClock::setShowCircle");
  if (m_showCircle == show) {
    return;
  }
  m_showCircle = show;
  m_face->setVisible(show);
  for (auto* tick : m_ticks) {
    tick->setVisible(show);
  }
}

void DesktopClock::setShowSeconds(bool show) {
  uiAssertNotRendering("DesktopClock::setShowSeconds");
  if (m_showSeconds == show) {
    return;
  }
  m_showSeconds = show;
  m_second->setVisible(show);
  updateHands();
}

void DesktopClock::updateHands() {
  const std::chrono::hh_mm_ss time{visibleTime(m_time, m_showSeconds)};
  const float hour = static_cast<float>(time.hours().count() % 12);
  const float minute = static_cast<float>(time.minutes().count());
  const float second = static_cast<float>(time.seconds().count());
  m_hour->setRotation((hour + minute / 60.0F + second / 3600.0F) * 30.0F * kRadiansPerDegree);
  m_minute->setRotation((minute + second / 60.0F) * 6.0F * kRadiansPerDegree);
  m_second->setRotation(second * 6.0F * kRadiansPerDegree);
}

void DesktopClock::layoutAnalog() {
  const float scale = m_contentScale;
  const float size = ::Style::fontSizeBody * 5.25F * scale;
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
  m_analogRoot->setSize(size, size);
  setSize(size, size);
}

void DesktopClock::setStyle(Style style) {
  uiAssertNotRendering("DesktopClock::setStyle");
  if (m_style == style) {
    return;
  }
  m_style = style;
  m_label->setVisible(style == Style::Digital);
  m_analogRoot->setVisible(style == Style::Analog);
}
