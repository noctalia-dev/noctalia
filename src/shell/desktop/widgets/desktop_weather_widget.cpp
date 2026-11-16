#include "shell/desktop/widgets/desktop_weather_widget.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>

namespace {

  constexpr float kMaxWidth = 180.0F;
  constexpr float kBaseHeight = 72.0F;
  constexpr float kGlyphTextGap = 13.0F;
  constexpr float kColumnGap = 8.0F;
  constexpr float kLineGap = 2.0F;
  constexpr float kForecastSectionGap = 6.0F;
  constexpr float kForecastRowHeight = 20.0F;
  constexpr float kForecastDayWidth = 34.0F;
  constexpr float kForecastGlyphSlotWidth = 24.0F;

  float temperatureFontSize(float contentScale) { return Style::fontSizeBody * 2.25F * contentScale; }
  float conditionFontSize(float contentScale) { return Style::fontSizeBody * contentScale; }
  float glyphFontSize(float contentScale) { return Style::fontSizeBody * 3.0F * contentScale; }
  float forecastFontSize(float contentScale) { return Style::fontSizeCaption * contentScale; }
  float forecastGlyphFontSize(float contentScale) { return Style::fontSizeBody * 1.25F * contentScale; }

  std::string todayIso(std::int32_t utcOffsetSeconds) {
    const auto now = std::chrono::system_clock::now() + std::chrono::seconds{utcOffsetSeconds};
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    return formatStrftime("%Y-%m-%d", tm);
  }

  std::string weekdayAbbrev(const std::string& isoDate) {
    if (isoDate.size() != 10) {
      return isoDate;
    }

    std::tm tm{};
    tm.tm_year = std::stoi(isoDate.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(isoDate.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(isoDate.substr(8, 2));
    if (std::mktime(&tm) == -1) {
      return isoDate;
    }

    const std::string weekday = formatStrftime("%a", tm);
    return weekday.empty() ? isoDate : weekday;
  }

} // namespace

namespace {

  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;

} // namespace

DesktopWeatherWidget::DesktopWeatherWidget(const WeatherService* weather, Options options)
    : m_weather(weather), m_color(options.color), m_shadow(options.shadow), m_showForecast(options.showForecast),
      m_forecastDays(std::clamp(options.forecastDays, 1, static_cast<int>(kMaxForecastRows))) {}

void DesktopWeatherWidget::create() {
  auto rootNode = ui::node({});

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
      .fontWeight = FontWeight::Bold,
      .color = m_color,
      .maxLines = 1,
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

  for (auto& row : m_forecastRows) {
    auto day = ui::label({
        .out = &row.day,
        .fontSize = forecastFontSize(contentScale()),
        .color = m_color,
        .maxLines = 1,
        .textAlign = TextAlign::Start,
        .visible = false,
    });
    rootNode->addChild(std::move(day));

    auto forecastGlyph = ui::glyph({
        .out = &row.glyph,
        .glyph = "weather-cloud",
        .glyphSize = forecastGlyphFontSize(contentScale()),
        .color = m_color,
        .visible = false,
    });
    rootNode->addChild(std::move(forecastGlyph));

    auto temps = ui::label({
        .out = &row.temps,
        .fontSize = forecastFontSize(contentScale()),
        .color = m_color,
        .maxLines = 1,
        .textAlign = TextAlign::Start,
        .visible = false,
    });
    rootNode->addChild(std::move(temps));
  }

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
      if (m_glyph != nullptr) {
        m_glyph->setColor(m_color);
      }
      if (m_temperature != nullptr) {
        m_temperature->setColor(m_color);
      }
      if (m_condition != nullptr) {
        m_condition->setColor(m_color);
      }
      for (auto& row : m_forecastRows) {
        if (row.glyph != nullptr) {
          row.glyph->setColor(m_color);
        }
        if (row.day != nullptr) {
          row.day->setColor(m_color);
        }
        if (row.temps != nullptr) {
          row.temps->setColor(m_color);
        }
      }
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      applyShadow();
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "show_forecast") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showForecast = *v;
      sync();
      syncForecast(renderer);
      layout(renderer);
      (void)allSettings;
      return true;
    }
    return false;
  }
  if (key == "forecast_days") {
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      m_forecastDays = std::clamp(static_cast<int>(*v), 1, static_cast<int>(kMaxForecastRows));
      syncForecast(renderer);
      layout(renderer);
      (void)allSettings;
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopWeatherWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_temperature != nullptr) {
    m_temperature->setFontFamily(family);
  }
  if (m_condition != nullptr) {
    m_condition->setFontFamily(family);
  }
  for (const auto& row : m_forecastRows) {
    if (row.day != nullptr) {
      row.day->setFontFamily(family);
    }
    if (row.temps != nullptr) {
      row.temps->setFontFamily(family);
    }
  }
}

void DesktopWeatherWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr || m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float maxWidth = kMaxWidth * scale;
  const float currentHeight = kBaseHeight * scale;
  const int forecastRowCount = m_showForecast ? std::clamp(m_forecastDays, 1, static_cast<int>(kMaxForecastRows)) : 0;
  const float forecastHeight = forecastRowCount > 0
      ? (kForecastSectionGap + static_cast<float>(forecastRowCount) * kForecastRowHeight) * scale
      : 0.0F;
  const float height = currentHeight + forecastHeight;

  // Glyph::measure keeps an existing width, so size the box explicitly at every scale; otherwise
  // the glyph stays at its first-layout width and the scaled icon spills outside its box.
  const float glyphBox = std::round(glyphFontSize(scale));
  m_glyph->setGlyphSize(glyphBox);
  m_glyph->setSize(glyphBox, glyphBox);
  applyShadow();

  sync();
  syncForecast(renderer);

  m_glyph->measure(renderer);

  const float textX = glyphBox + kGlyphTextGap * scale;
  const float textWidth = std::max(1.0F, maxWidth - textX);

  m_temperature->setFontSize(temperatureFontSize(scale));
  m_temperature->setMaxWidth(textWidth);
  m_temperature->setMaxLines(1);

  m_condition->setFontSize(conditionFontSize(scale));
  m_condition->setMaxWidth(textWidth);
  m_condition->setMaxLines(1);

  m_temperature->measure(renderer);
  m_condition->measure(renderer);

  const bool hasCondition = !m_condition->text().empty();
  const float lineGap = hasCondition ? kLineGap * scale : 0.0F;
  float textHeight = m_temperature->height();
  if (hasCondition) {
    textHeight += lineGap + m_condition->height();
  }
  float currentWidth = textX + m_temperature->width();
  if (hasCondition) {
    currentWidth = std::max(currentWidth, textX + m_condition->width());
  }

  const float dayWidth = kForecastDayWidth * scale;
  const float forecastGlyphSlot = kForecastGlyphSlotWidth * scale;
  const float tempsX = dayWidth + forecastGlyphSlot + kColumnGap * scale;
  float forecastWidth = 0.0F;
  for (int i = 0; i < forecastRowCount; ++i) {
    auto& row = m_forecastRows[static_cast<std::size_t>(i)];
    if (row.day == nullptr || row.glyph == nullptr || row.temps == nullptr) {
      continue;
    }
    row.day->setFontSize(forecastFontSize(scale));
    row.day->setMaxWidth(dayWidth);
    row.temps->setFontSize(forecastFontSize(scale));
    row.temps->setMaxWidth(std::max(1.0F, maxWidth - tempsX));
    const float forecastGlyphBox = std::round(forecastGlyphFontSize(scale));
    row.glyph->setGlyphSize(forecastGlyphBox);
    row.glyph->setSize(forecastGlyphBox, forecastGlyphBox);
    row.day->measure(renderer);
    row.temps->measure(renderer);
    row.glyph->measure(renderer);
    if (row.temps->visible()) {
      forecastWidth = std::max(forecastWidth, tempsX + row.temps->width());
    }
  }

  // Size to the drawn content and center each section in it, so neither the current conditions
  // nor the forecast rows leave a blank column on one side.
  const float width = std::max(currentWidth, forecastWidth);
  const float currentX = std::round((width - currentWidth) * 0.5F);
  const float forecastX = std::round((width - forecastWidth) * 0.5F);

  m_glyph->setPosition(currentX, std::round((currentHeight - m_glyph->height()) * 0.5F));

  float y = std::round((currentHeight - textHeight) * 0.5F);
  m_temperature->setPosition(currentX + textX, y);
  y += std::round(m_temperature->height() + lineGap);
  m_condition->setPosition(currentX + textX, y);

  const float rowHeight = kForecastRowHeight * scale;
  const float forecastStartY = currentHeight + kForecastSectionGap * scale;
  for (int i = 0; i < forecastRowCount; ++i) {
    auto& row = m_forecastRows[static_cast<std::size_t>(i)];
    if (row.day == nullptr || row.glyph == nullptr || row.temps == nullptr) {
      continue;
    }
    const float rowY = forecastStartY + static_cast<float>(i) * rowHeight;
    row.day->setPosition(forecastX, std::round(rowY + (rowHeight - row.day->height()) * 0.5F));
    row.glyph->setPosition(
        forecastX + dayWidth + std::round((forecastGlyphSlot - row.glyph->width()) * 0.5F),
        std::round(rowY + (rowHeight - row.glyph->height()) * 0.5F)
    );
    row.temps->setPosition(forecastX + tempsX, std::round(rowY + (rowHeight - row.temps->height()) * 0.5F));
  }

  // Include the shadow offset so a tight widget box fits the shadow instead of clipping it.
  const float shadowPad = m_shadow ? kShadowOffset * scale : 0.0F;
  root()->setSize(std::round(width + shadowPad), height + shadowPad);
}

void DesktopWeatherWidget::doUpdate(Renderer& renderer) {
  bool changed = sync();
  if (m_showForecast) {
    changed = syncForecast(renderer) || changed;
  }
  if (changed) {
    requestLayout();
  }
}

void DesktopWeatherWidget::applyShadow() {
  const ColorSpec shadow = colorSpecFromRole(ColorRole::Shadow, kShadowAlpha);
  const auto applyToLabel = [this, shadow](Label* label) {
    if (label == nullptr) {
      return;
    }
    if (m_shadow) {
      const float offset = kShadowOffset * contentScale();
      label->setShadow(shadow, offset, offset);
    } else {
      label->clearShadow();
    }
  };

  if (m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return;
  }
  if (m_shadow) {
    const float offset = kShadowOffset * contentScale();
    m_glyph->setShadow(shadow, offset, offset);
    m_temperature->setShadow(shadow, offset, offset);
    m_condition->setShadow(shadow, offset, offset);
  } else {
    m_glyph->clearShadow();
    m_temperature->clearShadow();
    m_condition->clearShadow();
  }

  for (const auto& row : m_forecastRows) {
    applyToLabel(row.day);
    applyToLabel(row.temps);
    if (row.glyph != nullptr) {
      if (m_shadow) {
        const float offset = kShadowOffset * contentScale();
        row.glyph->setShadow(shadow, offset, offset);
      } else {
        row.glyph->clearShadow();
      }
    }
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

bool DesktopWeatherWidget::syncForecast(Renderer& renderer) {
  const int rowCount = m_showForecast ? std::clamp(m_forecastDays, 1, static_cast<int>(kMaxForecastRows)) : 0;
  const bool hasWeatherData = m_weather != nullptr && m_weather->hasData();
  bool changed = false;

  for (std::size_t i = 0; i < kMaxForecastRows; ++i) {
    auto& row = m_forecastRows[i];
    if (row.day == nullptr || row.glyph == nullptr || row.temps == nullptr) {
      continue;
    }

    const bool visible = hasWeatherData && static_cast<int>(i) < rowCount;
    if (row.day->visible() != visible) {
      changed = true;
    }
    row.day->setVisible(visible);
    row.glyph->setVisible(visible);
    row.temps->setVisible(visible);
    if (!visible) {
      continue;
    }

    const auto& snapshot = m_weather->snapshot();
    const bool firstForecastIsToday =
        !snapshot.forecastDays.empty() && snapshot.forecastDays.front().dateIso == todayIso(snapshot.utcOffsetSeconds);
    const std::size_t forecastStart = firstForecastIsToday ? 1 : 0;
    const std::size_t dayIndex = forecastStart + i;
    if (dayIndex >= snapshot.forecastDays.size()) {
      row.day->setVisible(false);
      row.glyph->setVisible(false);
      row.temps->setVisible(false);
      changed = true;
      continue;
    }

    const auto& day = snapshot.forecastDays[dayIndex];
    const std::string dayText = weekdayAbbrev(day.dateIso);
    const std::string_view glyphName = WeatherService::glyphForCode(day.weatherCode, true);
    const std::string tempsText = std::format(
        "{} / {}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMinC))),
        static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMaxC))),
        m_weather->displayTemperatureUnit()
    );

    if (dayText != row.lastDay) {
      row.lastDay = dayText;
      row.day->setText(dayText);
      row.day->measure(renderer);
      changed = true;
    }
    if (glyphName != row.lastGlyph) {
      row.lastGlyph = glyphName;
      row.glyph->setGlyph(glyphName);
      row.glyph->measure(renderer);
      changed = true;
    }
    if (tempsText != row.lastTemps) {
      row.lastTemps = tempsText;
      row.temps->setText(tempsText);
      row.temps->measure(renderer);
      changed = true;
    }
  }

  return changed;
}
