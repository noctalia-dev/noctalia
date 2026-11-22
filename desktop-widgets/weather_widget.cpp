#include "weather_widget.h"

#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

} // namespace

namespace {
  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;

} // namespace

DesktopWeather::DesktopWeather() {
  auto glyph = ui::glyph({
      .out = &m_glyph,
      .glyph = "weather-cloud",
      .glyphSize = glyphFontSize(m_contentScale),
      .color = m_color,
  });
  addChild(std::move(glyph));

  auto temperature = ui::label({
      .out = &m_temperature,
      .fontSize = temperatureFontSize(m_contentScale),
      .fontWeight = FontWeight::Bold,
      .color = m_color,
      .maxLines = 1,
      .textAlign = TextAlign::Start,
  });
  addChild(std::move(temperature));

  auto condition = ui::label({
      .out = &m_condition,
      .fontSize = conditionFontSize(m_contentScale),
      .color = m_color,
      .maxLines = 1,
      .textAlign = TextAlign::Start,
  });
  addChild(std::move(condition));

  for (auto& row : m_forecastRows) {
    auto day = ui::label({
        .out = &row.day,
        .fontSize = forecastFontSize(m_contentScale),
        .color = m_color,
        .maxLines = 1,
        .textAlign = TextAlign::Start,
        .visible = false,
    });
    addChild(std::move(day));

    auto forecastGlyph = ui::glyph({
        .out = &row.glyph,
        .glyph = "weather-cloud",
        .glyphSize = forecastGlyphFontSize(m_contentScale),
        .color = m_color,
        .visible = false,
    });
    addChild(std::move(forecastGlyph));

    auto temps = ui::label({
        .out = &row.temps,
        .fontSize = forecastFontSize(m_contentScale),
        .color = m_color,
        .maxLines = 1,
        .textAlign = TextAlign::Start,
        .visible = false,
    });
    addChild(std::move(temps));
  }

  m_temperature->setText(m_data.temperature);
  applyShadow();
}

void DesktopWeather::setContentScale(float scale) {
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

void DesktopWeather::setShadow(bool enabled) {
  m_shadow = enabled;
  applyShadow();
  markLayoutDirty();
}

void DesktopWeather::setForecast(bool show, int days) {
  m_showForecast = show;
  m_forecastDays = std::clamp(days, 1, static_cast<int>(kMaxForecastRows));
  syncData();
  markLayoutDirty();
}

void DesktopWeather::setColor(const ColorSpec& color) {
  m_color = color;
  m_glyph->setColor(color);
  m_temperature->setColor(color);
  m_condition->setColor(color);
  for (auto& row : m_forecastRows) {
    row.day->setColor(color);
    row.glyph->setColor(color);
    row.temps->setColor(color);
  }
}

bool DesktopWeather::setData(Data data) {
  if (m_data == data) {
    return false;
  }
  m_data = std::move(data);
  m_glyph->setGlyph(m_data.glyph);
  m_temperature->setText(m_data.temperature);
  m_condition->setText(m_data.condition);
  syncData();
  markLayoutDirty();
  return true;
}

void DesktopWeather::setFontFamily(const std::string& family) {
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

void DesktopWeather::doLayout(Renderer& renderer) {
  if (m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return;
  }

  const float scale = m_contentScale;
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
  setSize(std::round(width + shadowPad), height + shadowPad);
}

void DesktopWeather::applyShadow() {
  const ColorSpec shadow = colorSpecFromRole(ColorRole::Shadow, kShadowAlpha);
  const auto applyToLabel = [this, shadow](Label* label) {
    if (label == nullptr) {
      return;
    }
    if (m_shadow) {
      const float offset = kShadowOffset * m_contentScale;
      label->setShadow(shadow, offset, offset);
    } else {
      label->clearShadow();
    }
  };

  if (m_glyph == nullptr || m_temperature == nullptr || m_condition == nullptr) {
    return;
  }
  if (m_shadow) {
    const float offset = kShadowOffset * m_contentScale;
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
        const float offset = kShadowOffset * m_contentScale;
        row.glyph->setShadow(shadow, offset, offset);
      } else {
        row.glyph->clearShadow();
      }
    }
  }
}

void DesktopWeather::syncData() {
  for (std::size_t i = 0; i < kMaxForecastRows; ++i) {
    auto& row = m_forecastRows[i];
    const bool visible = m_showForecast && i < static_cast<std::size_t>(m_forecastDays) && i < m_data.forecast.size();
    row.day->setVisible(visible);
    row.glyph->setVisible(visible);
    row.temps->setVisible(visible);
    if (visible) {
      const auto& day = m_data.forecast[i];
      row.day->setText(day.day);
      row.glyph->setGlyph(day.glyph);
      row.temps->setText(day.temperatures);
    }
  }
}
