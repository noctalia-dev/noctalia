#include "shell/bar/widgets/brightness_widget.h"

#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/brightness_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

  const char* brightnessGlyphName(float brightness) {
    if (brightness < 0.4f) {
      return "brightness-low";
    }
    return "brightness-high";
  }

  constexpr float kScrollStep = 0.05f;

} // namespace

BrightnessWidget::BrightnessWidget(BrightnessService* brightness, wl_output* output, bool showLabel)
    : m_brightness(brightness), m_output(output), m_showLabel(showLabel) {}

void BrightnessWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("control-center", "display"); });
  area->setOnAxis([this](const InputArea::PointerData& data) {
    if (m_brightness == nullptr) {
      return;
    }
    const auto* display = m_brightness->findByOutput(m_output);
    if (display == nullptr) {
      return;
    }
    const float delta = data.scrollDelta(1.0f) > 0 ? -kScrollStep : kScrollStep;
    const float newValue = std::clamp(display->brightness + delta, 0.0f, 1.0f);
    m_brightness->setBrightness(display->id, newValue);
  });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "brightness-high",
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .visible = m_showLabel,
      })
  );

  setRoot(std::move(area));
}

void BrightnessWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (m_glyph == nullptr || m_label == nullptr || rootNode == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  syncState(renderer);
  if (!rootNode->visible()) {
    rootNode->setParticipatesInLayout(false);
    return;
  }

  m_glyph->measure(renderer);
  if (m_label->visible()) {
    m_label->measure(renderer);
  }

  const bool labelVisible = m_label->visible();
  if (m_isVertical && labelVisible) {
    const float w = std::max(m_glyph->width(), m_label->width());
    m_glyph->setPosition(std::round((w - m_glyph->width()) * 0.5f), 0.0f);
    m_label->setPosition(std::round((w - m_label->width()) * 0.5f), m_glyph->height());
    rootNode->setSize(w, m_glyph->height() + m_label->height());
  } else {
    const float h = labelVisible ? std::max(m_glyph->height(), m_label->height()) : m_glyph->height();
    m_glyph->setPosition(0.0f, std::round((h - m_glyph->height()) * 0.5f));
    float totalWidth = m_glyph->width();
    if (labelVisible) {
      m_label->setPosition(m_glyph->width() + Style::spaceXs, std::round((h - m_label->height()) * 0.5f));
      totalWidth = m_label->x() + m_label->width();
    }
    rootNode->setSize(totalWidth, h);
  }
}

void BrightnessWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void BrightnessWidget::syncState(Renderer& renderer) {
  if (m_brightness == nullptr || m_glyph == nullptr || m_label == nullptr) {
    return;
  }

  auto* rootNode = root();
  const auto* display = m_brightness->findByOutput(m_output);
  if (display == nullptr) {
    m_lastAvailable = false;
    m_lastBrightness = -1.0f;
    if (rootNode != nullptr) {
      rootNode->setVisible(false);
      rootNode->setParticipatesInLayout(false);
    }
    return;
  }

  if (rootNode != nullptr) {
    rootNode->setVisible(true);
    rootNode->setParticipatesInLayout(true);
  }

  const float brightness = display->brightness;
  const bool becameAvailable = !m_lastAvailable;
  if (!becameAvailable && std::abs(brightness - m_lastBrightness) < 0.001f && m_isVertical == m_lastVertical) {
    return;
  }

  m_lastAvailable = true;
  m_lastBrightness = brightness;
  m_lastVertical = m_isVertical;

  m_glyph->setGlyph(brightnessGlyphName(brightness));
  m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
  m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);

  m_label->setVisible(m_showLabel);
  if (m_showLabel) {
    int pct = static_cast<int>(std::round(brightness * 100.0f));
    m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_label->setText(m_isVertical ? std::to_string(pct) : std::to_string(pct) + "%");
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
  }

  if (rootNode != nullptr) {
    int pct = static_cast<int>(std::round(brightness * 100.0f));
    std::vector<TooltipRow> rows;
    rows.push_back({"Brightness", std::to_string(pct) + "%"});
    if (!display->label.empty()) {
      rows.push_back({"Display", display->label});
    }
    static_cast<InputArea*>(rootNode)->setTooltip(std::move(rows));
  }

  requestRedraw();
}
