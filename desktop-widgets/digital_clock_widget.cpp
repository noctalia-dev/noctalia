#include "digital_clock_widget.h"

#include "core/ui_phase.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
  constexpr float kFontScale = 4.0F;
  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;
} // namespace

DigitalClockWidget::DigitalClockWidget() {
  setHitTestVisible(false);
  addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * kFontScale,
          .fontWeight = FontWeight::Bold,
          .fontFeatures = "tnum",
          .maxLines = 0,
          .textAlign = TextAlign::Center,
      })
  );
  applyShadow();
}

bool DigitalClockWidget::setText(std::string_view text) {
  uiAssertNotRendering("DigitalClockWidget::setText");
  return m_label->setText(text);
}

void DigitalClockWidget::setFontFamily(std::string family) {
  uiAssertNotRendering("DigitalClockWidget::setFontFamily");
  if (m_label->font().family == family) {
    return;
  }
  m_label->setFontFamily(std::move(family));
  m_metricsDirty = true;
}

void DigitalClockWidget::setColor(const ColorSpec& color) {
  uiAssertNotRendering("DigitalClockWidget::setColor");
  if (m_color == color) {
    return;
  }
  m_color = color;
  m_label->setColor(color);
}

void DigitalClockWidget::setShadow(bool enabled) {
  uiAssertNotRendering("DigitalClockWidget::setShadow");
  if (m_shadow == enabled) {
    return;
  }
  m_shadow = enabled;
  applyShadow();
  markLayoutDirty();
}

void DigitalClockWidget::setCenterText(bool center) {
  uiAssertNotRendering("DigitalClockWidget::setCenterText");
  if (m_centerText == center) {
    return;
  }
  m_centerText = center;
  m_label->setTextAlign(center ? TextAlign::Center : TextAlign::Start);
}

void DigitalClockWidget::setContentScale(float scale) {
  uiAssertNotRendering("DigitalClockWidget::setContentScale");
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

const std::string& DigitalClockWidget::text() const noexcept { return m_label->text(); }
const std::string& DigitalClockWidget::fontFamily() const noexcept { return m_label->font().family; }

void DigitalClockWidget::updateStableWidth(Renderer& renderer, float fontSize) {
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

void DigitalClockWidget::applyShadow() {
  if (m_shadow) {
    const float offset = kShadowOffset * m_contentScale;
    m_label->setShadow(colorSpecFromRole(ColorRole::Shadow, kShadowAlpha), offset, offset);
  } else {
    m_label->clearShadow();
  }
}

void DigitalClockWidget::doLayout(Renderer& renderer) {
  UiPhaseScope phase(UiPhase::Layout);
  const float fontSize = Style::fontSizeBody * kFontScale * m_contentScale;
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
