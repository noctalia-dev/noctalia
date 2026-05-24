#include "shell/control_center/weather_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/scene/effect_node.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>

using namespace control_center;

namespace {

  // Set to a specific effect to bypass weather-code detection. Reset to None when done testing.
  constexpr EffectType kTestEffect = EffectType::None;

  constexpr float kCurrentGlyphSize = Style::controlHeightLg * 2.2f;

  std::string windDirectionLabel(int degrees) {
    static constexpr std::array<const char*, 8> kDirs = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int normalized = ((degrees % 360) + 360) % 360;
    const int index = static_cast<int>(std::lround(normalized / 45.0)) % 8;
    return kDirs[static_cast<std::size_t>(index)];
  }

} // namespace

WeatherTab::WeatherTab(WeatherService* weather, ConfigService* config) : m_weather(weather), m_config(config) {
  m_detailRows.fill(nullptr);
  m_dayRows.fill(nullptr);
  m_daySeparators.fill(nullptr);
  m_dayIconSlots.fill(nullptr);
  m_dayGlyphs.fill(nullptr);
  m_dayMetas.fill(nullptr);
  m_dayDescs.fill(nullptr);
  m_dayTemps.fill(nullptr);
}

std::unique_ptr<Flex> WeatherTab::create() {
  const float scale = contentScale();
  auto tab = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto leftColumn = ui::column({
      .out = &m_leftColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 3.0f,
  });

  auto currentCard = ui::row({
      .out = &m_currentCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceXs * scale,
      .clipChildren = true,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
        card.setDirection(FlexDirection::Horizontal);
        card.setPadding(Style::spaceXs * scale, Style::spaceXs * scale);
        card.setGap(Style::spaceSm * scale);
      },
  });

  auto effectNode = std::make_unique<EffectNode>();
  effectNode->setParticipatesInLayout(false);
  effectNode->setZIndex(-1);
  effectNode->setVisible(false);
  effectNode->setRadius(Style::scaledRadiusXl(scale));
  m_effectNode = static_cast<EffectNode*>(currentCard->addChild(std::move(effectNode)));

  auto glyphColumn = ui::row(
      {.out = &m_glyphColumn,
       .align = FlexAlign::Center,
       .justify = FlexJustify::End,
       .fillHeight = true,
       .flexGrow = 0.9f},
      ui::glyph({
          .out = &m_currentGlyph,
          .glyph = "weather-cloud",
          .glyphSize = kCurrentGlyphSize * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );
  currentCard->addChild(std::move(glyphColumn));

  auto currentText = ui::column(
      {.out = &m_currentText,
       .align = FlexAlign::Stretch,
       .justify = FlexJustify::Center,
       .gap = Style::spaceXs * scale,
       .fillWidth = true,
       .flexGrow = 1.0f}
  );

  currentText->addChild(
      ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
          ui::label({
              .out = &m_currentTempLabel,
              .text = "--°C",
              .fontSize = Style::fontSizeTitle * 2.35f * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .fontWeight = FontWeight::Bold,
          }),
          ui::label({
              .out = &m_currentHiLoLabel,
              .text = "--↑ --↓",
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
              .maxLines = 1,
          })
      )
  );

  currentText->addChild(
      ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.5f * scale},
          ui::label({
              .out = &m_currentDescLabel,
              .text = i18n::tr("control-center.weather.waiting"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
          }),
          ui::label({
              .out = &m_updatedLabel,
              .text = " ",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .configure = [](Label& label) { label.setCaptionStyle(); },
          }),
          ui::label({
              .out = &m_statusLabel,
              .text = " ",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .visible = false,
              .configure = [](Label& label) { label.setCaptionStyle(); },
          })
      )
  );

  currentCard->addChild(std::move(currentText));
  leftColumn->addChild(std::move(currentCard));

  auto detailsCard = ui::column({
      .out = &m_detailsCard,
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
        card.setPadding(Style::spaceMd * scale, Style::spaceMd * scale, Style::spaceLg * scale, Style::spaceMd * scale);
        card.setGap(0.0f);
      },
  });
  const float detailKeyWidth = Style::controlHeightLg * 2.0f * scale;

  std::size_t detailRowIndex = 0;
  auto addDetailRow = [&](std::string_view iconName, std::string_view key, Label*& valueOut) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = (Style::spaceSm + Style::spaceXs) * scale,
        .minHeight = Style::controlHeightSm * scale,
        .flexGrow = 0.0f,
    });
    if (detailRowIndex < kDetailRowCount) {
      m_detailRows[detailRowIndex] = row.get();
    }
    ++detailRowIndex;

    row->addChild(
        ui::glyph({
            .glyph = std::string(iconName),
            .glyphSize = (Style::fontSizeBody + Style::spaceXs) * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    row->addChild(
        ui::label({
            .text = std::string(key),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .minWidth = detailKeyWidth - (Style::fontSizeBody + Style::spaceXs) * scale - Style::spaceSm * scale,
        })
    );
    row->addChild(
        ui::label({
            .out = &valueOut,
            .text = "--",
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .fontWeight = FontWeight::Bold,
            .textAlign = TextAlign::End,
            .flexGrow = 1.0f,
        })
    );
    detailsCard->addChild(std::move(row));
  };

  addDetailRow("temperature-sun", i18n::tr("control-center.weather.details.tempMax"), m_tempMaxLabel);
  addDetailRow("temperature", i18n::tr("control-center.weather.details.tempMin"), m_tempMinLabel);
  addDetailRow("wind", i18n::tr("control-center.weather.details.wind"), m_windLabel);
  addDetailRow("weather-sunrise", i18n::tr("control-center.weather.details.sunrise"), m_sunriseLabel);
  addDetailRow("weather-sunset", i18n::tr("control-center.weather.details.sunset"), m_sunsetLabel);
  addDetailRow("mountain", i18n::tr("control-center.weather.details.elevation"), m_elevationLabel);
  addDetailRow("clock", i18n::tr("control-center.weather.details.timezone"), m_timeZoneLabel);

  leftColumn->addChild(std::move(detailsCard));

  tab->addChild(std::move(leftColumn));

  auto forecastColumn = ui::column({
      .out = &m_forecastColumn,
      .gap = 0.0f,
      .fillHeight = true,
      .flexGrow = 2.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& column) {
        applySectionCardStyle(column, scale, opacity, borders);
        column.setGap(0.0f);
        column.setPadding(0.0f, Style::spaceMd * scale);
      },
  });

  for (std::size_t i = 0; i < kDayCount; ++i) {
    auto row = ui::column(
        {.out = &m_dayRows[i],
         .align = FlexAlign::Stretch,
         .justify = FlexJustify::Center,
         .gap = Style::spaceXs * 0.5f * scale,
         .flexGrow = 1.0f,
         .configure = [scale](Flex& dayRow) { dayRow.setPadding(Style::spaceXs * scale, 0.0f); }}
    );

    auto daySlot = ui::row(
        {.out = &m_dayIconSlots[i], .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
        ui::glyph({
            .out = &m_dayGlyphs[i],
            .glyph = "weather-cloud",
            .glyphSize = Style::fontSizeBody * 1.2f * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        }),
        ui::label({
            .out = &m_dayMetas[i],
            .text = i18n::tr("control-center.weather.forecast-placeholder.day"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .fontWeight = FontWeight::Bold,
        })
    );
    auto topRow = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm * scale,
    });
    topRow->addChild(std::move(daySlot));

    topRow->addChild(
        ui::label({
            .out = &m_dayTemps[i],
            .text = i18n::tr("control-center.weather.forecast-placeholder.temperature"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .textAlign = TextAlign::End,
        })
    );

    row->addChild(std::move(topRow));
    row->addChild(
        ui::label({
            .out = &m_dayDescs[i],
            .text = i18n::tr("control-center.weather.forecast-placeholder.description"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    forecastColumn->addChild(std::move(row));

    if (i + 1 < kDayCount) {
      forecastColumn->addChild(
          ui::separator({
              .out = &m_daySeparators[i],
              .thickness = std::max(1.0f, scale),
          })
      );
    }
  }

  tab->addChild(std::move(forecastColumn));
  return tab;
}

void WeatherTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr || m_currentText == nullptr || m_forecastColumn == nullptr) {
    return;
  }

  for (auto* label : m_dayTemps) {
    if (label != nullptr) {
      label->setMaxWidth(0.0f);
      label->setMinWidth(0.0f);
    }
  }

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  const float leftColumnWidth =
      m_leftColumn != nullptr
          ? std::max(0.0f, m_leftColumn->width() - (m_leftColumn->paddingLeft() + m_leftColumn->paddingRight()))
          : contentWidth;
  if (m_currentCard != nullptr) {
    m_currentCard->setMinWidth(leftColumnWidth);
  }
  if (m_detailsCard != nullptr) {
    m_detailsCard->setMinWidth(leftColumnWidth);
  }
  if (m_statusLabel != nullptr) {
    m_statusLabel->setMaxWidth(leftColumnWidth);
  }
  for (auto* label :
       {m_windLabel, m_sunriseLabel, m_sunsetLabel, m_tempMaxLabel, m_tempMinLabel, m_elevationLabel,
        m_timeZoneLabel}) {
    if (label != nullptr) {
      label->setMaxWidth(leftColumnWidth);
    }
  }

  const float scale = contentScale();

  if (m_currentCard != nullptr) {
    m_currentCard->setMinHeight(Style::controlHeightLg * 3.1f * scale);
  }
  if (m_detailsCard != nullptr) {
    m_detailsCard->setMinHeight(0.0f);
    m_detailsCard->setFlexGrow(0.0f);
  }

  if (m_currentGlyph != nullptr && m_currentCard != nullptr) {
    const float cardInnerHeight =
        std::max(0.0f, m_currentCard->height() - (m_currentCard->paddingTop() + m_currentCard->paddingBottom()));
    const float desiredGlyph =
        std::max(Style::controlHeightLg * 1.8f * scale, std::min(kCurrentGlyphSize * scale, cardInnerHeight * 0.8f));
    m_currentGlyph->setGlyphSize(desiredGlyph);
  }

  if (m_detailsCard != nullptr) {
    const float rowMinHeight = Style::controlHeightSm * scale;
    for (auto* row : m_detailRows) {
      if (row != nullptr) {
        row->setMinHeight(row->visible() ? rowMinHeight : 0.0f);
        row->setFlexGrow(0.0f);
      }
    }
  }

  std::size_t visibleForecastDays = 0;
  for (std::size_t i = 0; i < kDayCount; ++i) {
    if (m_dayRows[i] != nullptr && m_dayRows[i]->visible()) {
      ++visibleForecastDays;
    }
  }

  if (m_forecastColumn != nullptr && visibleForecastDays > 0) {
    const float separatorThickness = std::max(1.0f, scale);
    std::size_t visibleSeparators = 0;
    for (auto* separator : m_daySeparators) {
      if (separator != nullptr) {
        separator->setThickness(separatorThickness);
        if (separator->visible()) {
          ++visibleSeparators;
        }
      }
    }
    const float forecastInnerHeight = std::max(
        0.0f, m_forecastColumn->height() - (m_forecastColumn->paddingTop() + m_forecastColumn->paddingBottom())
    );
    const float separatorsTotal = separatorThickness * static_cast<float>(visibleSeparators);
    const float rowHeight = std::max(
        Style::controlHeightLg * scale,
        (forecastInnerHeight - separatorsTotal) / static_cast<float>(visibleForecastDays)
    );

    for (std::size_t i = 0; i < kDayCount; ++i) {
      if (m_dayRows[i] == nullptr) {
        continue;
      }
      m_dayRows[i]->setMinHeight(m_dayRows[i]->visible() ? rowHeight : 0.0f);
    }
  }

  float forecastTempColumnWidth = 0.0f;
  for (std::size_t i = 0; i < kDayCount; ++i) {
    if (m_dayRows[i] != nullptr && m_dayRows[i]->visible() && m_dayTemps[i] != nullptr) {
      m_dayTemps[i]->measure(renderer);
      forecastTempColumnWidth = std::max(forecastTempColumnWidth, m_dayTemps[i]->width());
    }
  }

  const float forecastInnerWidth =
      m_forecastColumn != nullptr
          ? std::max(
                0.0f, m_forecastColumn->width() - m_forecastColumn->paddingLeft() - m_forecastColumn->paddingRight()
            )
          : 0.0f;
  for (std::size_t i = 0; i < kDayCount; ++i) {
    if (m_dayRows[i] == nullptr || !m_dayRows[i]->visible()) {
      continue;
    }
    if (m_dayTemps[i] != nullptr) {
      m_dayTemps[i]->setMinWidth(forecastTempColumnWidth);
    }
    if (m_dayMetas[i] != nullptr) {
      const float glyphWidth = m_dayGlyphs[i] != nullptr ? m_dayGlyphs[i]->width() : 0.0f;
      const float daySlotGap = m_dayIconSlots[i] != nullptr ? m_dayIconSlots[i]->gap() : 0.0f;
      const float topRowGap = Style::spaceSm * scale;
      const float metaMaxWidth = forecastInnerWidth - forecastTempColumnWidth - topRowGap - glyphWidth - daySlotGap;
      m_dayMetas[i]->setMaxWidth(std::max(1.0f, metaMaxWidth));
    }
  }

  if (m_effectNode != nullptr && m_currentCard != nullptr) {
    m_effectNode->setPosition(0.0f, 0.0f);
    m_effectNode->setFrameSize(m_currentCard->width(), m_currentCard->height());
  }

  // The weather tab derives several width constraints from the first measurement
  // pass. Run layout again so the final geometry reflects those constraints
  // instead of keeping the placeholder/pre-constraint positions.
  m_rootLayout->layout(renderer);

  if (m_effectNode != nullptr && m_currentCard != nullptr) {
    m_effectNode->setFrameSize(m_currentCard->width(), m_currentCard->height());
  }
}

void WeatherTab::doUpdate(Renderer& renderer) { sync(renderer); }

void WeatherTab::setForecastVisibleDayCount(std::size_t count) {
  const std::size_t visibleCount = std::min(count, kDayCount);
  for (std::size_t i = 0; i < kDayCount; ++i) {
    if (m_dayRows[i] != nullptr) {
      m_dayRows[i]->setVisible(i < visibleCount);
    }
    if (i + 1 < kDayCount && m_daySeparators[i] != nullptr) {
      m_daySeparators[i]->setVisible(i + 1 < visibleCount);
    }
  }
}

void WeatherTab::onClose() {
  m_rootLayout = nullptr;
  m_leftColumn = nullptr;
  m_currentCard = nullptr;
  m_glyphColumn = nullptr;
  m_detailsCard = nullptr;
  m_currentText = nullptr;
  m_forecastColumn = nullptr;
  m_statusLabel = nullptr;
  m_currentGlyph = nullptr;
  m_currentTempLabel = nullptr;
  m_currentHiLoLabel = nullptr;
  m_currentDescLabel = nullptr;
  m_updatedLabel = nullptr;
  m_windLabel = nullptr;
  m_sunriseLabel = nullptr;
  m_sunsetLabel = nullptr;
  m_tempMaxLabel = nullptr;
  m_tempMinLabel = nullptr;
  m_elevationLabel = nullptr;
  m_timeZoneLabel = nullptr;
  m_detailRows.fill(nullptr);
  m_dayRows.fill(nullptr);
  m_daySeparators.fill(nullptr);
  m_dayIconSlots.fill(nullptr);
  m_dayGlyphs.fill(nullptr);
  m_dayMetas.fill(nullptr);
  m_dayDescs.fill(nullptr);
  m_dayTemps.fill(nullptr);
  m_effectNode = nullptr;
  m_activeEffect = EffectType::None;
  m_shaderTime = 0.0f;
}

void WeatherTab::sync(Renderer& renderer) {
  if (m_statusLabel == nullptr || m_currentGlyph == nullptr || m_currentTempLabel == nullptr ||
      m_currentDescLabel == nullptr || m_updatedLabel == nullptr) {
    return;
  }

  const bool showLocation = m_config == nullptr || m_config->config().shell.showLocation;
  if (m_updatedLabel != nullptr) {
    m_updatedLabel->setVisible(showLocation);
  }

  if (m_weather == nullptr || !m_weather->enabled()) {
    m_currentGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    m_currentTempLabel->setText("--°C");
    if (m_currentHiLoLabel != nullptr) {
      m_currentHiLoLabel->setText("-- / --");
    }
    m_currentDescLabel->setText(i18n::tr("control-center.weather.disabled"));
    m_updatedLabel->setText(i18n::tr("control-center.weather.location-unavailable"));
    m_updatedLabel->setVisible(false);
    m_statusLabel->setText("");
    m_statusLabel->setVisible(false);
    if (m_windLabel != nullptr) {
      m_windLabel->setText("--");
    }
    if (m_sunriseLabel != nullptr) {
      m_sunriseLabel->setText("--");
    }
    if (m_sunsetLabel != nullptr) {
      m_sunsetLabel->setText("--");
    }
    if (m_tempMaxLabel != nullptr) {
      m_tempMaxLabel->setText("--");
    }
    if (m_tempMinLabel != nullptr) {
      m_tempMinLabel->setText("--");
    }
    if (m_elevationLabel != nullptr) {
      m_elevationLabel->setText("--");
    }
    if (m_timeZoneLabel != nullptr) {
      m_timeZoneLabel->setText("--");
    }
    setForecastVisibleDayCount(0);
    hideEffect();
    return;
  }

  if (!m_weather->locationConfigured()) {
    m_currentGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    m_currentTempLabel->setText(std::format("--{}", m_weather->displayTemperatureUnit()));
    if (m_currentHiLoLabel != nullptr) {
      m_currentHiLoLabel->setText("-- / --");
    }
    m_currentDescLabel->setText(i18n::tr("control-center.weather.configure-location"));
    m_updatedLabel->setText(i18n::tr("control-center.weather.location-unavailable"));
    m_updatedLabel->setVisible(false);
    m_statusLabel->setText("");
    m_statusLabel->setVisible(false);
    if (m_windLabel != nullptr) {
      m_windLabel->setText("--");
    }
    if (m_sunriseLabel != nullptr) {
      m_sunriseLabel->setText("--");
    }
    if (m_sunsetLabel != nullptr) {
      m_sunsetLabel->setText("--");
    }
    if (m_tempMaxLabel != nullptr) {
      m_tempMaxLabel->setText("--");
    }
    if (m_tempMinLabel != nullptr) {
      m_tempMinLabel->setText("--");
    }
    if (m_elevationLabel != nullptr) {
      m_elevationLabel->setText("--");
    }
    if (m_timeZoneLabel != nullptr) {
      m_timeZoneLabel->setText("--");
    }
    setForecastVisibleDayCount(0);
    hideEffect();
    return;
  }

  const auto& snapshot = m_weather->snapshot();
  if (!snapshot.valid) {
    m_currentGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    m_currentTempLabel->setText(std::format("--{}", m_weather->displayTemperatureUnit()));
    if (m_currentHiLoLabel != nullptr) {
      m_currentHiLoLabel->setText("-- / --");
    }
    m_currentDescLabel->setText(
        m_weather->loading() ? i18n::tr("control-center.weather.fetching")
                             : i18n::tr("control-center.weather.data-unavailable")
    );
    m_updatedLabel->setText(
        snapshot.locationName.empty() ? i18n::tr("weather.locations.current") : snapshot.locationName
    );
    m_updatedLabel->setVisible(false);
    m_statusLabel->setText(m_weather->error());
    m_statusLabel->setVisible(!m_weather->error().empty());
    if (m_windLabel != nullptr) {
      m_windLabel->setText("--");
    }
    if (m_sunriseLabel != nullptr) {
      m_sunriseLabel->setText("--");
    }
    if (m_sunsetLabel != nullptr) {
      m_sunsetLabel->setText("--");
    }
    if (m_tempMaxLabel != nullptr) {
      m_tempMaxLabel->setText("--");
    }
    if (m_tempMinLabel != nullptr) {
      m_tempMinLabel->setText("--");
    }
    if (m_elevationLabel != nullptr) {
      m_elevationLabel->setText("--");
    }
    if (m_timeZoneLabel != nullptr) {
      m_timeZoneLabel->setText("--");
    }
    setForecastVisibleDayCount(0);
    hideEffect();
    return;
  }

  m_currentGlyph->setGlyph(WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay));
  m_currentGlyph->setColor(colorSpecFromRole(snapshot.current.isDay ? ColorRole::Primary : ColorRole::Secondary));
  m_currentTempLabel->setText(
      std::format(
          "{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC))),
          m_weather->displayTemperatureUnit()
      )
  );
  if (m_currentHiLoLabel != nullptr) {
    if (!snapshot.forecastDays.empty()) {
      m_currentHiLoLabel->setText(
          std::format(
              "{} / {}{}",
              static_cast<int>(
                  std::lround(m_weather->displayTemperature(snapshot.forecastDays.front().temperatureMaxC))
              ),
              static_cast<int>(
                  std::lround(m_weather->displayTemperature(snapshot.forecastDays.front().temperatureMinC))
              ),
              m_weather->displayTemperatureUnit()
          )
      );
    } else {
      m_currentHiLoLabel->setText("-- / --");
    }
  }
  m_currentDescLabel->setText(WeatherService::descriptionForCode(snapshot.current.weatherCode));
  m_updatedLabel->setText(
      snapshot.locationName.empty() ? i18n::tr("weather.locations.current") : snapshot.locationName
  );
  m_updatedLabel->setVisible(showLocation);
  const std::string status = m_weather->loading() ? i18n::tr("control-center.weather.refreshing")
                                                  : (snapshot.valid ? std::string{} : m_weather->error());
  m_statusLabel->setText(status);
  m_statusLabel->setColor(
      colorSpecFromRole(m_weather->error().empty() ? ColorRole::OnSurfaceVariant : ColorRole::Error)
  );
  m_statusLabel->setVisible(!status.empty());
  if (m_windLabel != nullptr) {
    const bool imperial = m_weather->useImperial();
    const double windSpeed = imperial ? snapshot.current.windSpeedKmh * 0.621371 : snapshot.current.windSpeedKmh;
    const char* windUnit =
        imperial ? "mph" : (snapshot.currentUnits.windSpeed.empty() ? "km/h" : snapshot.currentUnits.windSpeed.c_str());
    m_windLabel->setText(
        std::format(
            "{} {} {}", static_cast<int>(std::lround(windSpeed)), windUnit,
            windDirectionLabel(snapshot.current.windDirectionDeg)
        )
    );
  }
  if (m_sunriseLabel != nullptr) {
    const auto& fmt = m_config->config().shell.timeFormat;
    m_sunriseLabel->setText(
        !snapshot.forecastDays.empty() ? formatIsoTime(snapshot.forecastDays.front().sunriseIso, fmt.c_str())
                                       : std::string("--")
    );
  }
  if (m_sunsetLabel != nullptr) {
    const auto& fmt = m_config->config().shell.timeFormat;
    m_sunsetLabel->setText(
        !snapshot.forecastDays.empty() ? formatIsoTime(snapshot.forecastDays.front().sunsetIso, fmt.c_str())
                                       : std::string("--")
    );
  }
  auto unit = m_weather->displayTemperatureUnit();
  if (m_tempMaxLabel != nullptr) {
    if (!snapshot.forecastDays.empty()) {
      const int temp =
          static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.forecastDays.front().temperatureMaxC)));
      m_tempMaxLabel->setText(std::format("{}{}", temp, unit));
    } else {
      m_tempMaxLabel->setText("--");
    }
  }
  if (m_tempMinLabel != nullptr) {
    if (!snapshot.forecastDays.empty()) {
      const int temp =
          static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.forecastDays.front().temperatureMinC)));
      m_tempMinLabel->setText(std::format("{}{}", temp, unit));
    } else {
      m_tempMinLabel->setText("--");
    }
  }
  if (m_elevationLabel != nullptr) {
    const bool imperial = m_weather->useImperial();
    const int elevation = static_cast<int>(imperial ? snapshot.elevationM * 3.28084 : snapshot.elevationM);
    m_elevationLabel->setText(std::format("{}{}", elevation, imperial ? "ft" : "m"));
  }
  if (m_timeZoneLabel != nullptr) {
    // Use the last component of the IANA path ("America/Toronto" → "Toronto") to keep
    // the label short enough to remain right-aligned without elision in most cases.
    std::string tzCity = snapshot.timezone;
    if (const auto slash = tzCity.rfind('/'); slash != std::string::npos) {
      tzCity = tzCity.substr(slash + 1);
    }
    m_timeZoneLabel->setText(
        snapshot.timezoneAbbreviation.empty() ? (snapshot.timezone.empty() ? std::string("--") : snapshot.timezone)
                                              : std::format("{} ({})", snapshot.timezoneAbbreviation, tzCity)
    );
  }

  const bool firstForecastIsToday =
      !snapshot.forecastDays.empty() && snapshot.forecastDays.front().dateIso == todayIso(snapshot.utcOffsetSeconds);
  const std::size_t forecastStart = firstForecastIsToday ? 1 : 0;
  const std::size_t visibleForecastCount = forecastStart < snapshot.forecastDays.size()
                                               ? std::min(kDayCount, snapshot.forecastDays.size() - forecastStart)
                                               : 0;

  setForecastVisibleDayCount(visibleForecastCount);
  for (std::size_t i = 0; i < kDayCount; ++i) {
    const bool visible = i < visibleForecastCount;
    if (!visible) {
      continue;
    }

    const auto& day = snapshot.forecastDays[i + forecastStart];
    if (m_dayGlyphs[i] != nullptr) {
      m_dayGlyphs[i]->setGlyph(WeatherService::glyphForCode(day.weatherCode, true));
      m_dayGlyphs[i]->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_dayGlyphs[i]->measure(renderer);
    }
    if (m_dayMetas[i] != nullptr) {
      m_dayMetas[i]->setText(weekdayLabel(day.dateIso));
      m_dayMetas[i]->measure(renderer);
    }
    if (m_dayTemps[i] != nullptr) {
      m_dayTemps[i]->setText(
          std::format(
              "{} / {}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMaxC))),
              static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMinC))),
              m_weather->displayTemperatureUnit()
          )
      );
      m_dayTemps[i]->measure(renderer);
    }
    if (m_dayDescs[i] != nullptr) {
      m_dayDescs[i]->setText(WeatherService::shortDescriptionForCode(day.weatherCode));
      m_dayDescs[i]->measure(renderer);
    }
  }

  if (m_effectNode != nullptr) {
    const EffectType newEffect =
        kTestEffect != EffectType::None
            ? kTestEffect
            : (m_weather->effectsEnabled() ? effectForWeatherCode(snapshot.current.weatherCode, snapshot.current.isDay)
                                           : EffectType::None);
    if (newEffect != m_activeEffect) {
      m_activeEffect = newEffect;
      m_shaderTime = 0.0f;
    }
    m_effectNode->setEffectType(m_activeEffect);
    m_effectNode->setBgColor(colorForRole(ColorRole::Surface));
    m_effectNode->setRadius(Style::scaledRadiusXl(contentScale()));
    m_effectNode->setVisible(m_activeEffect != EffectType::None);
  }
}

std::string WeatherTab::todayIso(std::int32_t utcOffsetSeconds) {
  const auto now = std::chrono::system_clock::now() + std::chrono::seconds{utcOffsetSeconds};
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&time, &tm);

  return formatStrftime("%Y-%m-%d", tm);
}

std::string WeatherTab::weekdayLabel(const std::string& isoDate) {
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

  const std::string weekday = formatStrftime("%A", tm);
  if (weekday.empty()) {
    return isoDate;
  }
  return weekday;
}

void WeatherTab::hideEffect() {
  m_activeEffect = EffectType::None;
  m_shaderTime = 0.0f;
  if (m_effectNode != nullptr) {
    m_effectNode->setEffectType(EffectType::None);
    m_effectNode->setVisible(false);
  }
}

void WeatherTab::onFrameTick(float deltaMs) {
  if (m_effectNode == nullptr || !m_effectNode->visible() || m_activeEffect == EffectType::None) {
    return;
  }
  m_shaderTime += deltaMs * 0.001f;
  m_effectNode->setTime(m_shaderTime);
}

EffectType WeatherTab::effectForWeatherCode(std::int32_t code, bool isDay) {
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    return EffectType::Rain;
  }
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    return EffectType::Snow;
  }
  if (code == 3) {
    return EffectType::Cloud;
  }
  if (code >= 40 && code <= 49) {
    return EffectType::Fog;
  }
  if (code == 0 && isDay) {
    return EffectType::Sun;
  }
  if (code == 0 && !isDay) {
    return EffectType::Stars;
  }
  return EffectType::None;
}
