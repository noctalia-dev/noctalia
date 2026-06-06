#include "shell/desktop/widgets/desktop_weather_widget.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "system/weather_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>

namespace {

  constexpr float kBaseWidth = 180.0f;
  constexpr float kBaseHeight = 72.0f;
  constexpr float kGlyphSlotWidth = 52.0f;
  constexpr float kColumnGap = 8.0f;
  constexpr float kLineGap = 2.0f;

  float temperatureFontSize(float contentScale) { return Style::fontSizeBody * 2.25f * contentScale; }
  float conditionFontSize(float contentScale) { return Style::fontSizeBody * contentScale; }
  float glyphFontSize(float contentScale) { return Style::fontSizeBody * 3.0f * contentScale; }

} // namespace

namespace {

  constexpr float kShadowAlpha = 0.6f;
  constexpr float kShadowOffset = 1.5f;

} // namespace

DesktopWeatherWidget::DesktopWeatherWidget(const WeatherService* weather, ColorSpec color, bool shadow)
    : m_weather(weather), m_color(color), m_shadow(shadow) {}

void DesktopWeatherWidget::create() {
  auto rootNode = std::make_unique<Node>();
  rootNode->setClipChildren(true);

  auto glyph = ui::glyph({
      .out = &m_glyph,
      .glyph = "weather-cloud",
      .glyphSize = glyphFontSize(contentScale()),
      .color = m_color,
  });
  rootNode->addChild(std::move(glyph));

  auto temperature = ui::label({
      .out = &m_temperature,
      .fontSize = temperatureFontSize(contentScale()),
      .color = m_color,
      .maxLines = 1,
      .fontWeight = FontWeight::Bold,
      .textAlign = TextAlign::Start,
  });
  rootNode->addChild(std::move(temperature));

  auto condition = ui::label({
      .out = &m_condition,
      .fontSize = conditionFontSize(contentScale()),
      .color = m_color,
      .maxLines = 1,
      .textAlign = TextAlign::Start,
  });
  rootNode->addChild(std::move(condition));

  setRoot(std::move(rootNode));
  applyShadow();
}

bool DesktopWeatherWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_color = colorSpecFromConfigString(*v, key);
      if (m_glyph != nullptr)
        m_glyph->setColor(m_color);
      if (m_temperature != nullptr)
        m_temperature->setColor(m_color);
      if (m_condition != nullptr)
        m_condition->setColor(m_color);
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      applyShadow();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopWeatherWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr || m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float width = kBaseWidth * scale;
  const float height = kBaseHeight * scale;
  const float glyphSlotWidth = kGlyphSlotWidth * scale;
  const float textX = glyphSlotWidth + kColumnGap * scale;
  const float textWidth = std::max(1.0f, width - textX);

  m_temperature->setFontSize(temperatureFontSize(scale));
  m_temperature->setMaxWidth(textWidth);
  m_temperature->setMaxLines(1);

  m_condition->setFontSize(conditionFontSize(scale));
  m_condition->setMaxWidth(textWidth);
  m_condition->setMaxLines(1);

  m_glyph->setGlyphSize(glyphFontSize(scale));
  applyShadow();

  sync();

  m_temperature->measure(renderer);
  m_condition->measure(renderer);
  m_glyph->measure(renderer);

  const bool hasCondition = !m_condition->text().empty();
  const float lineGap = hasCondition ? kLineGap * scale : 0.0f;
  float textHeight = m_temperature->height();
  if (hasCondition) {
    textHeight += lineGap + m_condition->height();
  }

  m_glyph->setPosition(
      std::round((glyphSlotWidth - m_glyph->width()) * 0.5f), std::round((height - m_glyph->height()) * 0.5f)
  );

  float y = std::round((height - textHeight) * 0.5f);
  m_temperature->setPosition(textX, y);
  y += std::round(m_temperature->height() + lineGap);
  m_condition->setPosition(textX, y);

  root()->setSize(width, height);
}

void DesktopWeatherWidget::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (sync()) {
    requestLayout();
  }
}

void DesktopWeatherWidget::applyShadow() {
  if (m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return;
  }
  if (m_shadow) {
    const float offset = kShadowOffset * contentScale();
    const Color shadow(0.0f, 0.0f, 0.0f, kShadowAlpha);
    m_glyph->setShadow(shadow, offset, offset);
    m_temperature->setShadow(shadow, offset, offset);
    m_condition->setShadow(shadow, offset, offset);
  } else {
    m_glyph->clearShadow();
    m_temperature->clearShadow();
    m_condition->clearShadow();
  }
}

bool DesktopWeatherWidget::sync() {
  if (m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return false;
  }

  std::string glyphName = "weather-cloud";
  std::string temperatureText = "--";
  std::string conditionText;

  if (m_weather == nullptr || !m_weather->enabled()) {
    temperatureText = i18n::tr("desktop-widgets.weather.off");
  } else if (!m_weather->locationConfigured()) {
    temperatureText = i18n::tr("desktop-widgets.weather.no-location");
  } else if (m_weather->hasData()) {
    const auto& snapshot = m_weather->snapshot();
    glyphName = WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
    const int temp = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
    temperatureText = std::format("{}{}", temp, m_weather->displayTemperatureUnit());
    conditionText = WeatherService::shortDescriptionForCode(snapshot.current.weatherCode);
  } else if (m_weather->loading()) {
    temperatureText = i18n::tr("desktop-widgets.weather.loading");
  } else if (!m_weather->error().empty()) {
    temperatureText = i18n::tr("desktop-widgets.weather.error");
  }

  bool changed = false;

  if (glyphName != m_lastGlyph) {
    m_lastGlyph = glyphName;
    m_glyph->setGlyph(glyphName);
    changed = true;
  }

  if (temperatureText != m_lastTemperature) {
    m_lastTemperature = temperatureText;
    m_temperature->setText(temperatureText);
    changed = true;
  }

  if (conditionText != m_lastCondition) {
    m_lastCondition = conditionText;
    m_condition->setText(conditionText);
    changed = true;
  }

  return changed;
}
