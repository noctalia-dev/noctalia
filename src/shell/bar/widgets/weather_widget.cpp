#include "shell/bar/widgets/weather_widget.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "system/weather_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>

WeatherWidget::WeatherWidget(WeatherService* weather, wl_output* /*output*/, float maxWidth, bool showCondition)
    : m_weather(weather), m_maxWidth(maxWidth), m_showCondition(showCondition) {}

void WeatherWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("control-center", "weather"); });
  m_area = area.get();

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "weather-cloud",
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .maxWidth = m_maxWidth * m_contentScale,
          .fontWeight = labelFontWeight(),
      })
  );

  setRoot(std::move(area));
}

void WeatherWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_glyph == nullptr || m_label == nullptr || root() == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  sync(renderer);

  m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
  m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  m_label->setTextAlign(m_isVertical ? TextAlign::Center : TextAlign::Start);
  m_label->setMaxWidth(m_isVertical ? containerWidth : (m_maxWidth * m_contentScale));
  m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_label->measure(renderer);

  const float spacing = m_label->text().empty() ? 0.0f : (Style::spaceXs * m_contentScale);
  if (m_isVertical) {
    const float contentWidth = std::max(m_glyph->width(), m_label->width());
    m_glyph->setPosition(std::round((contentWidth - m_glyph->width()) * 0.5f), 0.0f);
    m_label->setPosition(std::round((contentWidth - m_label->width()) * 0.5f), m_glyph->height() + spacing);
    root()->setSize(contentWidth, m_label->y() + m_label->height());
  } else {
    const float contentHeight = std::max(m_glyph->height(), m_label->height());
    const float glyphY = std::round((contentHeight - m_glyph->height()) * 0.5f);
    const float labelY = std::round((contentHeight - m_label->height()) * 0.5f);
    m_glyph->setPosition(0.0f, glyphY);
    m_label->setPosition(m_glyph->width() + spacing, labelY);
    root()->setSize(m_label->x() + m_label->width(), contentHeight);
  }
}

void WeatherWidget::doUpdate(Renderer& renderer) { sync(renderer); }

void WeatherWidget::sync(Renderer& renderer) {
  if (m_glyph == nullptr || m_label == nullptr) {
    return;
  }

  auto verticalTemperature = [](int temp) { return std::format("{}\xC2\xB0", temp); };

  std::string glyph = "weather-cloud";
  std::string text =
      m_isVertical ? i18n::tr("bar.widgets.weather.vertical-default") : i18n::tr("bar.widgets.weather.default");

  if (m_weather == nullptr || !m_weather->enabled()) {
    text = m_isVertical ? i18n::tr("bar.widgets.weather.vertical-off") : i18n::tr("bar.widgets.weather.off");
  } else if (!m_weather->locationConfigured()) {
    text = m_isVertical ? i18n::tr("bar.widgets.weather.vertical-no-location")
                        : i18n::tr("bar.widgets.weather.no-location");
  } else if (m_weather->hasData()) {
    const auto& snapshot = m_weather->snapshot();
    glyph = WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
    const int temp = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
    const std::string unit = m_weather->displayTemperatureUnit();
    if (m_isVertical) {
      text = verticalTemperature(temp);
    } else {
      text = std::format("{}{}", temp, unit);
    }
    if (m_showCondition && !m_isVertical) {
      text += " ";
      text += WeatherService::shortDescriptionForCode(snapshot.current.weatherCode);
    }
  } else if (m_weather->loading()) {
    text = m_isVertical ? i18n::tr("bar.widgets.weather.vertical-loading") : i18n::tr("bar.widgets.weather.loading");
  } else if (!m_weather->error().empty()) {
    text = m_isVertical ? i18n::tr("bar.widgets.weather.vertical-error") : i18n::tr("bar.widgets.weather.error");
  }

  bool changed = false;

  if (glyph != m_lastGlyph) {
    m_lastGlyph = glyph;
    m_glyph->setGlyph(glyph);
    m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
    changed = true;
  }

  if (text != m_lastText) {
    m_lastText = text;
    m_label->setText(text);
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
    changed = true;
  }

  if (changed) {
    requestRedraw();
  }
}
