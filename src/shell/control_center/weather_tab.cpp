#include "shell/control_center/weather_tab.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/scene/effect_node.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_content.h"
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
  m_forecastRows.fill(nullptr);
  m_forecastSeparators.fill(nullptr);
  m_forecastIconSlots.fill(nullptr);
  m_forecastGlyphs.fill(nullptr);
  m_forecastMetas.fill(nullptr);
  m_forecastDescs.fill(nullptr);
  m_forecastTemps.fill(nullptr);
  m_forecastHitAreas.fill(nullptr);
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
          }),
          ui::label({
              .out = &m_statusLabel,
              .text = " ",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .visible = false,
          })
      )
  );

  currentCard->addChild(std::move(currentText));

  auto locationPrompt = ui::row(
      {.out = &m_locationPrompt,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .gap = Style::spaceMd * scale,
       .fillWidth = true,
       .fillHeight = true,
       .flexGrow = 1.0f,
       .visible = false},
      ui::glyph({
          .out = &m_locationPromptGlyph,
          .glyph = "map-pin-off",
          .glyphSize = Style::controlHeightLg * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::column(
          {.align = FlexAlign::Stretch, .justify = FlexJustify::Center, .gap = Style::spaceXs * scale},
          ui::label({
              .text = i18n::tr("control-center.weather.no-location-title"),
              .fontSize = Style::fontSizeBody * 1.1f * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .fontWeight = FontWeight::Bold,
          }),
          ui::label({
              .out = &m_locationPromptBody,
              .text = i18n::tr("control-center.weather.no-location-body"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 2,
          })
      )
  );
  currentCard->addChild(std::move(locationPrompt));

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

  addDetailRow("temperature-sun", i18n::tr("control-center.weather.details.temp-max"), m_tempMaxLabel);
  addDetailRow("temperature", i18n::tr("control-center.weather.details.temp-min"), m_tempMinLabel);
  addDetailRow("wind", i18n::tr("control-center.weather.details.wind"), m_windLabel);
  addDetailRow("weather-sunrise", i18n::tr("control-center.weather.details.sunrise"), m_sunriseLabel);
  addDetailRow("weather-sunset", i18n::tr("control-center.weather.details.sunset"), m_sunsetLabel);
  addDetailRow("mountain", i18n::tr("control-center.weather.details.elevation"), m_elevationLabel);
  addDetailRow("sun", i18n::tr("control-center.weather.details.uv-index"), m_uvIndexLabel);
  addDetailRow("clock", i18n::tr("control-center.weather.details.timezone"), m_timeZoneLabel);

  leftColumn->addChild(std::move(detailsCard));

  tab->addChild(std::move(leftColumn));

  auto forecastColumn = ui::column({
      .out = &m_forecastColumn,
      .gap = Style::spaceXs * scale,
      .fillHeight = true,
      .flexGrow = 2.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& column) {
        applySectionCardStyle(column, scale, opacity, borders);
        column.setGap(Style::spaceXs * scale);
        column.setPadding(Style::spaceMd * scale, Style::spaceMd * scale);
      },
  });

  forecastColumn->addChild(
      ui::segmented({
          .out = &m_forecastViewPicker,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("control-center.weather.forecast-view.daily")},
                  {.label = i18n::tr("control-center.weather.forecast-view.hourly")},
              },
          .selectedIndex = static_cast<std::size_t>(m_forecastView),
          .fontSize = Style::fontSizeCaption * scale,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t idx) {
            const ForecastView nextView = idx == 1 ? ForecastView::Hourly : ForecastView::Daily;
            if (m_forecastView == nextView) {
              return;
            }
            m_forecastView = nextView;
            PanelManager::instance().refresh();
          },
      })
  );

  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    auto row = ui::column(
        {.out = &m_forecastRows[i],
         .align = FlexAlign::Stretch,
         .justify = FlexJustify::Center,
         .gap = Style::spaceXs * 0.5f * scale,
         .flexGrow = 1.0f,
         .configure = [scale](Flex& dayRow) { dayRow.setPadding(Style::spaceXs * scale, 0.0f); }}
    );

    auto daySlot = ui::row(
        {.out = &m_forecastIconSlots[i], .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
        ui::glyph({
            .out = &m_forecastGlyphs[i],
            .glyph = "weather-cloud",
            .glyphSize = Style::fontSizeBody * 1.2f * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        }),
        ui::label({
            .out = &m_forecastMetas[i],
            .text = i18n::tr("control-center.weather.forecast-placeholder.day"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
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
            .out = &m_forecastTemps[i],
            .text = i18n::tr("control-center.weather.forecast-placeholder.temperature"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .textAlign = TextAlign::End,
        })
    );

    row->addChild(std::move(topRow));
    row->addChild(
        ui::label({
            .out = &m_forecastDescs[i],
            .text = i18n::tr("control-center.weather.forecast-placeholder.description"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );

    auto hitArea = std::make_unique<InputArea>();
    hitArea->setAcceptedButtons(0);
    hitArea->setParticipatesInLayout(false);
    hitArea->setZIndex(2);
    m_forecastHitAreas[i] = static_cast<InputArea*>(row->addChild(std::move(hitArea)));

    forecastColumn->addChild(std::move(row));

    if (i + 1 < kForecastRowCount) {
      forecastColumn->addChild(
          ui::separator({
              .out = &m_forecastSeparators[i],
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

  for (auto* label : m_forecastTemps) {
    if (label != nullptr) {
      label->setMaxWidth(0.0f);
      label->setMinWidth(0.0f);
    }
  }

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  const float leftColumnWidth = m_leftColumn != nullptr
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
       {m_windLabel, m_sunriseLabel, m_sunsetLabel, m_tempMaxLabel, m_tempMinLabel, m_elevationLabel, m_uvIndexLabel,
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
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (m_forecastRows[i] != nullptr && m_forecastRows[i]->visible()) {
      ++visibleForecastDays;
    }
  }

  if (m_forecastColumn != nullptr && visibleForecastDays > 0) {
    const float separatorThickness = std::max(1.0f, scale);
    std::size_t visibleSeparators = 0;
    for (auto* separator : m_forecastSeparators) {
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
    const float pickerHeight =
        m_forecastViewPicker != nullptr && m_forecastViewPicker->visible() ? m_forecastViewPicker->height() : 0.0f;
    const float separatorsTotal = separatorThickness * static_cast<float>(visibleSeparators);
    const float gapsTotal = m_forecastColumn->gap() * static_cast<float>(visibleForecastDays + visibleSeparators);
    const float rowHeight = std::max(
        Style::controlHeightLg * scale,
        (forecastInnerHeight - pickerHeight - separatorsTotal - gapsTotal) / static_cast<float>(visibleForecastDays)
    );

    for (std::size_t i = 0; i < kForecastRowCount; ++i) {
      if (m_forecastRows[i] == nullptr) {
        continue;
      }
      m_forecastRows[i]->setMinHeight(m_forecastRows[i]->visible() ? rowHeight : 0.0f);
    }
  }

  float forecastTempColumnWidth = 0.0f;
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (m_forecastRows[i] != nullptr && m_forecastRows[i]->visible() && m_forecastTemps[i] != nullptr) {
      m_forecastTemps[i]->measure(renderer);
      forecastTempColumnWidth = std::max(forecastTempColumnWidth, m_forecastTemps[i]->width());
    }
  }

  const float forecastInnerWidth = m_forecastColumn != nullptr
      ? std::max(0.0f, m_forecastColumn->width() - m_forecastColumn->paddingLeft() - m_forecastColumn->paddingRight())
      : 0.0f;
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (m_forecastRows[i] == nullptr || !m_forecastRows[i]->visible()) {
      continue;
    }
    if (m_forecastTemps[i] != nullptr) {
      m_forecastTemps[i]->setMinWidth(forecastTempColumnWidth);
    }
    if (m_forecastMetas[i] != nullptr) {
      const float glyphWidth = m_forecastGlyphs[i] != nullptr ? m_forecastGlyphs[i]->width() : 0.0f;
      const float daySlotGap = m_forecastIconSlots[i] != nullptr ? m_forecastIconSlots[i]->gap() : 0.0f;
      const float topRowGap = Style::spaceSm * scale;
      const float metaMaxWidth = forecastInnerWidth - forecastTempColumnWidth - topRowGap - glyphWidth - daySlotGap;
      m_forecastMetas[i]->setMaxWidth(std::max(1.0f, metaMaxWidth));
    }
    if (m_forecastDescs[i] != nullptr) {
      m_forecastDescs[i]->setMaxWidth(std::max(1.0f, forecastInnerWidth));
    }
  }

  if (m_locationPrompt != nullptr && m_locationPrompt->visible() && m_locationPromptBody != nullptr) {
    const float cardPadding =
        m_currentCard != nullptr ? m_currentCard->paddingLeft() + m_currentCard->paddingRight() : 0.0f;
    const float glyphWidth = m_locationPromptGlyph != nullptr ? m_locationPromptGlyph->width() : 0.0f;
    const float promptGap = Style::spaceMd * scale;
    const float textWidth = std::max(1.0f, leftColumnWidth - cardPadding - glyphWidth - promptGap);
    m_locationPromptBody->setMaxWidth(textWidth);
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

  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (m_forecastRows[i] == nullptr || m_forecastHitAreas[i] == nullptr) {
      continue;
    }
    const bool visible = m_forecastRows[i]->visible();
    m_forecastHitAreas[i]->setVisible(visible);
    m_forecastHitAreas[i]->setPosition(0.0f, 0.0f);
    m_forecastHitAreas[i]->setSize(
        visible ? m_forecastRows[i]->width() : 0.0f, visible ? m_forecastRows[i]->height() : 0.0f
    );
  }
}

void WeatherTab::doUpdate(Renderer& renderer) { sync(renderer); }

void WeatherTab::setForecastVisibleRowCount(std::size_t count) {
  const std::size_t visibleCount = std::min(count, kForecastRowCount);
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (m_forecastRows[i] != nullptr) {
      m_forecastRows[i]->setVisible(i < visibleCount);
    }
    if (m_forecastHitAreas[i] != nullptr) {
      m_forecastHitAreas[i]->setVisible(i < visibleCount);
      if (i >= visibleCount) {
        m_forecastHitAreas[i]->clearTooltip();
      }
    }
    if (i + 1 < kForecastRowCount && m_forecastSeparators[i] != nullptr) {
      m_forecastSeparators[i]->setVisible(i + 1 < visibleCount);
    }
  }
}

void WeatherTab::showLocationPrompt(bool show) {
  if (m_locationPrompt != nullptr) {
    m_locationPrompt->setVisible(show);
  }
  if (m_glyphColumn != nullptr) {
    m_glyphColumn->setVisible(!show);
  }
  if (m_currentText != nullptr) {
    m_currentText->setVisible(!show);
  }
}

void WeatherTab::onClose() {
  m_rootLayout = nullptr;
  m_leftColumn = nullptr;
  m_currentCard = nullptr;
  m_glyphColumn = nullptr;
  m_detailsCard = nullptr;
  m_currentText = nullptr;
  m_locationPrompt = nullptr;
  m_locationPromptGlyph = nullptr;
  m_locationPromptBody = nullptr;
  m_forecastColumn = nullptr;
  m_forecastViewPicker = nullptr;
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
  m_uvIndexLabel = nullptr;
  m_timeZoneLabel = nullptr;
  m_detailRows.fill(nullptr);
  m_forecastRows.fill(nullptr);
  m_forecastSeparators.fill(nullptr);
  m_forecastIconSlots.fill(nullptr);
  m_forecastGlyphs.fill(nullptr);
  m_forecastMetas.fill(nullptr);
  m_forecastDescs.fill(nullptr);
  m_forecastTemps.fill(nullptr);
  m_forecastHitAreas.fill(nullptr);
  m_effectNode = nullptr;
  m_activeEffect = EffectType::None;
  m_shaderTime = 0.0f;
}

void WeatherTab::sync(Renderer& renderer) {
  if (m_statusLabel == nullptr
      || m_currentGlyph == nullptr
      || m_currentTempLabel == nullptr
      || m_currentDescLabel == nullptr
      || m_updatedLabel == nullptr) {
    return;
  }

  showLocationPrompt(false);

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
    if (m_uvIndexLabel != nullptr) {
      m_uvIndexLabel->setText("--");
    }
    if (m_timeZoneLabel != nullptr) {
      m_timeZoneLabel->setText("--");
    }
    setForecastVisibleRowCount(0);
    hideEffect();
    return;
  }

  if (!m_weather->locationConfigured()) {
    showLocationPrompt(true);
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
    if (m_uvIndexLabel != nullptr) {
      m_uvIndexLabel->setText("--");
    }
    if (m_timeZoneLabel != nullptr) {
      m_timeZoneLabel->setText("--");
    }
    setForecastVisibleRowCount(0);
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
        snapshot.locationName.empty() ? i18n::tr("location.locations.current") : snapshot.locationName
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
    if (m_uvIndexLabel != nullptr) {
      m_uvIndexLabel->setText("--");
    }
    if (m_timeZoneLabel != nullptr) {
      m_timeZoneLabel->setText("--");
    }
    setForecastVisibleRowCount(0);
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
      snapshot.locationName.empty() ? i18n::tr("location.locations.current") : snapshot.locationName
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
  if (m_uvIndexLabel != nullptr) {
    m_uvIndexLabel->setText(std::format("{:.1f}", snapshot.current.uvIndex));
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

  if (m_forecastViewPicker != nullptr) {
    m_forecastViewPicker->setSelectedIndex(static_cast<std::size_t>(m_forecastView));
  }
  if (m_forecastView == ForecastView::Hourly) {
    syncHourlyForecast(renderer, snapshot);
  } else {
    syncDailyForecast(renderer, snapshot);
  }

  if (m_effectNode != nullptr) {
    const EffectType newEffect = kTestEffect != EffectType::None
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

void WeatherTab::syncDailyForecast(Renderer& renderer, const WeatherSnapshot& snapshot) {
  const bool firstForecastIsToday =
      !snapshot.forecastDays.empty() && snapshot.forecastDays.front().dateIso == todayIso(snapshot.utcOffsetSeconds);
  const std::size_t forecastStart = firstForecastIsToday ? 1 : 0;
  const std::size_t visibleForecastCount = forecastStart < snapshot.forecastDays.size()
      ? std::min(kForecastRowCount, snapshot.forecastDays.size() - forecastStart)
      : 0;

  setForecastVisibleRowCount(visibleForecastCount);
  const std::string timeFormat = m_config != nullptr && !m_config->config().shell.timeFormat.empty()
      ? m_config->config().shell.timeFormat
      : std::string("%H:%M");
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (i >= visibleForecastCount) {
      continue;
    }

    const auto& day = snapshot.forecastDays[i + forecastStart];
    const std::string condition = WeatherService::shortDescriptionForCode(day.weatherCode);
    const std::string tempHigh = std::format(
        "{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMaxC))),
        m_weather->displayTemperatureUnit()
    );
    const std::string tempLow = std::format(
        "{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMinC))),
        m_weather->displayTemperatureUnit()
    );
    const std::string sunrise =
        day.sunriseIso.empty() ? std::string("--") : formatIsoTime(day.sunriseIso, timeFormat.c_str());
    const std::string sunset =
        day.sunsetIso.empty() ? std::string("--") : formatIsoTime(day.sunsetIso, timeFormat.c_str());
    const std::vector<TooltipRow> tooltipRows{
        {i18n::tr("control-center.weather.daily.tooltip.condition"), condition},
        {i18n::tr("control-center.weather.daily.tooltip.high"), tempHigh},
        {i18n::tr("control-center.weather.daily.tooltip.low"), tempLow},
        {i18n::tr("control-center.weather.daily.tooltip.sunrise"), sunrise},
        {i18n::tr("control-center.weather.daily.tooltip.sunset"), sunset},
    };
    if (m_forecastHitAreas[i] != nullptr) {
      m_forecastHitAreas[i]->setTooltip(tooltipRows);
    }
    if (m_forecastGlyphs[i] != nullptr) {
      m_forecastGlyphs[i]->setGlyph(WeatherService::glyphForCode(day.weatherCode, true));
      m_forecastGlyphs[i]->setColor(colorSpecFromRole(ColorRole::Primary));
      m_forecastGlyphs[i]->measure(renderer);
    }
    if (m_forecastMetas[i] != nullptr) {
      m_forecastMetas[i]->setText(weekdayLabel(day.dateIso));
      m_forecastMetas[i]->clearTooltip();
      m_forecastMetas[i]->measure(renderer);
    }
    if (m_forecastTemps[i] != nullptr) {
      m_forecastTemps[i]->setText(std::format("{} / {}", tempHigh, tempLow));
      m_forecastTemps[i]->clearTooltip();
      m_forecastTemps[i]->measure(renderer);
    }
    if (m_forecastDescs[i] != nullptr) {
      m_forecastDescs[i]->setText(condition);
      m_forecastDescs[i]->clearTooltip();
      m_forecastDescs[i]->measure(renderer);
    }
  }
}

void WeatherTab::syncHourlyForecast(Renderer& renderer, const WeatherSnapshot& snapshot) {
  const std::size_t visibleForecastCount = std::min(kForecastRowCount, snapshot.forecastHours.size());
  const bool imperial = m_weather != nullptr && m_weather->useImperial();
  const char* windUnit =
      imperial ? "mph" : (snapshot.hourlyUnits.windSpeed.empty() ? "km/h" : snapshot.hourlyUnits.windSpeed.c_str());
  const std::string timeFormat = m_config != nullptr && !m_config->config().shell.timeFormat.empty()
      ? m_config->config().shell.timeFormat
      : std::string("%H:%M");

  setForecastVisibleRowCount(visibleForecastCount);
  for (std::size_t i = 0; i < kForecastRowCount; ++i) {
    if (i >= visibleForecastCount) {
      continue;
    }

    const auto& hour = snapshot.forecastHours[i];
    const int displayTemp = static_cast<int>(std::lround(m_weather->displayTemperature(hour.temperatureC)));
    const double displayWind = imperial ? hour.windSpeedKmh * 0.621371 : hour.windSpeedKmh;
    const std::string displayWindText = std::format("{} {}", static_cast<int>(std::lround(displayWind)), windUnit);
    const std::string condition = WeatherService::shortDescriptionForCode(hour.weatherCode);
    const std::vector<TooltipRow> tooltipRows{
        {i18n::tr("control-center.weather.hourly.tooltip.condition"), condition},
        {i18n::tr("control-center.weather.hourly.tooltip.rain"),
         std::format("{}%", hour.precipitationProbabilityPercent)},
        {i18n::tr("control-center.weather.hourly.tooltip.humidity"), std::format("{}%", hour.relativeHumidityPercent)},
        {i18n::tr("control-center.weather.hourly.tooltip.wind"), displayWindText},
    };
    if (m_forecastHitAreas[i] != nullptr) {
      m_forecastHitAreas[i]->setTooltip(tooltipRows);
    }
    if (m_forecastGlyphs[i] != nullptr) {
      m_forecastGlyphs[i]->setGlyph(WeatherService::glyphForCode(hour.weatherCode, hour.isDay));
      m_forecastGlyphs[i]->setColor(colorSpecFromRole(hour.isDay ? ColorRole::Primary : ColorRole::Secondary));
      m_forecastGlyphs[i]->measure(renderer);
    }
    if (m_forecastMetas[i] != nullptr) {
      m_forecastMetas[i]->setText(hourLabel(hour.timeIso, timeFormat));
      m_forecastMetas[i]->clearTooltip();
      m_forecastMetas[i]->measure(renderer);
    }
    if (m_forecastTemps[i] != nullptr) {
      m_forecastTemps[i]->setText(std::format("{}{}", displayTemp, m_weather->displayTemperatureUnit()));
      m_forecastTemps[i]->clearTooltip();
      m_forecastTemps[i]->measure(renderer);
    }
    if (m_forecastDescs[i] != nullptr) {
      m_forecastDescs[i]->setText(
          i18n::tr(
              "control-center.weather.hourly.summary", "condition", condition, "precip",
              hour.precipitationProbabilityPercent
          )
      );
      m_forecastDescs[i]->clearTooltip();
      m_forecastDescs[i]->measure(renderer);
    }
  }
}

std::string WeatherTab::todayIso(std::int32_t utcOffsetSeconds) {
  const auto now = std::chrono::system_clock::now() + std::chrono::seconds{utcOffsetSeconds};
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&time, &tm);

  return formatStrftime("%Y-%m-%d", tm);
}

std::string WeatherTab::hourLabel(const std::string& isoTime, const std::string& timeFormat) {
  if (isoTime.size() < 16) {
    return isoTime;
  }
  const std::string formatted = formatIsoTime(isoTime, timeFormat.empty() ? "%H:%M" : timeFormat.c_str());
  return formatted.empty() ? isoTime : formatted;
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
