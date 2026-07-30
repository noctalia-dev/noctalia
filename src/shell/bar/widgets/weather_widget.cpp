#include "shell/bar/widgets/weather_widget.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/tooltip/tooltip_content.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>

namespace {

  std::string windDirectionLabel(int degrees) {
    static constexpr std::array<const char*, 8> kDirs = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int normalized = ((degrees % 360) + 360) % 360;
    const int index = static_cast<int>(std::lround(normalized / 45.0)) % 8;
    return kDirs[static_cast<std::size_t>(index)];
  }

} // namespace

WeatherWidget::WeatherWidget(WeatherService* weather, wl_output* /*output*/, Options options)
    : m_weather(weather), m_maxWidth(static_cast<float>(options.maxWidth)), m_showCondition(options.showCondition),
      m_showTemperature(options.showTemperature) {}

void WeatherWidget::create() {
  auto area = ui::inputArea({});
  m_area = area.get();

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "weather-cloud",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .maxWidth = m_maxWidth * m_contentScale,
          .maxLines = 1,
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

  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
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

  constexpr const char* kVerticalUnavailable = "-";
  constexpr const char* kVerticalLoading = "...";

  auto verticalTemperature = [](int temp) { return std::format("{}\xC2\xB0", temp); };

  std::string glyph = "weather-cloud";
  std::string text = m_isVertical ? kVerticalUnavailable : i18n::tr("bar.widgets.weather.default");

  if (m_weather == nullptr || !m_weather->enabled()) {
    text = m_isVertical ? kVerticalUnavailable : i18n::tr("bar.widgets.weather.off");
  } else if (!m_weather->locationConfigured()) {
    text = m_isVertical ? kVerticalUnavailable : i18n::tr("bar.widgets.weather.no-location");
  } else if (m_weather->hasData()) {
    const auto& snapshot = m_weather->snapshot();
    glyph = WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
    const int temp = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
    const std::string unit = m_weather->displayTemperatureUnit();
    if (m_showTemperature) {
      if (m_isVertical) {
        text = verticalTemperature(temp);
      } else {
        text = std::format("{}{}", temp, unit);
      }
    } else {
      text.clear();
    }
    if (m_showCondition && !m_isVertical) {
      text += text.empty() ? "" : " ";
      text += WeatherService::shortDescriptionForCode(snapshot.current.weatherCode);
    }
  } else if (m_weather->loading()) {
    text = m_isVertical ? kVerticalLoading : i18n::tr("bar.widgets.weather.loading");
  } else if (!m_weather->error().empty()) {
    text = m_isVertical ? kVerticalUnavailable : i18n::tr("bar.widgets.weather.error");
  }

  bool changed = false;

  if (glyph != m_lastGlyph) {
    m_lastGlyph = glyph;
    m_glyph->setGlyph(glyph);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
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

  if (m_area == nullptr) {
    return;
  }

  if (m_weather == nullptr || !m_weather->hasData()) {
    m_area->clearTooltip();
    return;
  }

  const auto& snapshot = m_weather->snapshot();
  const bool imperial = m_weather->useImperial();
  const char* tempUnit = m_weather->displayTemperatureUnit();
  const std::string windUnit = imperial ? "mph" : "km/h";

  const int currentTemp = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
  const double displayWind = imperial ? snapshot.current.windSpeedKmh * 0.621371 : snapshot.current.windSpeedKmh;

  std::vector<TooltipRow> rows;
  rows.push_back(
      {i18n::tr("bar.widgets.weather.tooltip.condition"),
       WeatherService::shortDescriptionForCode(snapshot.current.weatherCode)}
  );
  rows.push_back({i18n::tr("bar.widgets.weather.tooltip.temperature"), std::format("{}{}", currentTemp, tempUnit)});

  if (!snapshot.forecastDays.empty()) {
    const auto& today = snapshot.forecastDays.front();
    const int high = static_cast<int>(std::lround(m_weather->displayTemperature(today.temperatureMaxC)));
    const int low = static_cast<int>(std::lround(m_weather->displayTemperature(today.temperatureMinC)));
    rows.push_back({i18n::tr("bar.widgets.weather.tooltip.high"), std::format("{}{}", high, tempUnit)});
    rows.push_back({i18n::tr("bar.widgets.weather.tooltip.low"), std::format("{}{}", low, tempUnit)});
  }

  rows.push_back(
      {i18n::tr("bar.widgets.weather.tooltip.humidity"), std::format("{}%", snapshot.current.relativeHumidityPercent)}
  );
  rows.push_back(
      {i18n::tr("bar.widgets.weather.tooltip.wind"),
       std::format(
           "{} {} {}", static_cast<int>(std::lround(displayWind)), windUnit,
           windDirectionLabel(snapshot.current.windDirectionDeg)
       )}
  );
  rows.push_back({i18n::tr("bar.widgets.weather.tooltip.uv"), std::format("{:.1f}", snapshot.current.uvIndex)});

  if (!snapshot.forecastDays.empty()) {
    const auto& today = snapshot.forecastDays.front();
    if (!today.sunriseIso.empty()) {
      rows.push_back({i18n::tr("bar.widgets.weather.tooltip.sunrise"), formatIsoTime(today.sunriseIso, "%H:%M")});
    }
    if (!today.sunsetIso.empty()) {
      rows.push_back({i18n::tr("bar.widgets.weather.tooltip.sunset"), formatIsoTime(today.sunsetIso, "%H:%M")});
    }
  }

  m_area->setTooltip(std::move(rows));
}
