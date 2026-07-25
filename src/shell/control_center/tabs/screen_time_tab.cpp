#include "shell/control_center/tabs/screen_time_tab.h"

#include "i18n/i18n.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_content.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "system/internal_app_metadata.h"
#include "system/screen_time_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace control_center;

namespace {

  constexpr float kChartHeight = 132.0f;
  constexpr float kBarGap = 2.0f;
  constexpr float kAppIconSize = 28.0f;
  constexpr float kUsageBarHeight = 4.0f;
  constexpr float kLegendSwatch = 6.0f;
  constexpr float kUsageDurationFontScale = 0.85f;

  IconResolver g_iconResolver;

  [[nodiscard]] std::string snapshotKey(int rangeDays, const ScreenTimeSnapshot& snapshot) {
    std::string key = std::to_string(rangeDays) + '|' + std::to_string(snapshot.total.count());
    key += snapshot.hourlyBuckets ? "|h" : "|d";
    for (const auto& bucket : snapshot.buckets) {
      key += ':';
      key += std::to_string(bucket.count());
    }
    for (const auto& series : snapshot.chartSeries) {
      key += '|';
      key += series.appKey;
      key += ':';
      key += std::to_string(series.total.count());
      for (const auto& bucket : series.buckets) {
        key += ':';
        key += std::to_string(bucket.count());
      }
    }
    for (const auto& app : snapshot.apps) {
      key += '|';
      key += app.appKey;
      key += '@';
      key += std::to_string(app.total.count());
    }
    return key;
  }

  Label* makeSectionHeader(Flex& parent, const std::string& text, float scale) {
    Label* ptr = nullptr;
    parent.addChild(
        ui::label({
            .out = &ptr,
            .text = text,
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    return ptr;
  }

  [[nodiscard]] float usageDurationFontSize(float scale) {
    return Style::fontSizeMini * scale * kUsageDurationFontScale;
  }

  [[nodiscard]] std::vector<TooltipRow> appUsageTooltip(const std::string& displayName, std::chrono::seconds duration) {
    return {{displayName, formatDuration(duration)}};
  }

  [[nodiscard]] std::uint32_t hashAppKey(std::string_view appKey) {
    std::uint32_t hash = 2166136261U;
    for (const unsigned char byte : appKey) {
      hash ^= byte;
      hash *= 16777619U;
    }
    return hash;
  }

  [[nodiscard]] Color appSeriesColor(std::string_view appKey) {
    const Color primary = colorForRole(ColorRole::Primary);
    float baseH = 0.0f;
    float baseS = 0.0f;
    float baseV = 0.0f;
    rgbToHsv(primary, baseH, baseS, baseV);

    const float hashT = static_cast<float>(hashAppKey(appKey) % 1000U) / 1000.0f;
    const float hue = baseH + hashT * 0.72f;
    const float sat = std::clamp(baseS * (0.82f + hashT * 0.22f), 0.38f, 0.82f);
    const float val = std::clamp(baseV * (0.86f + (1.0f - hashT) * 0.14f), 0.52f, 0.90f);
    return hsv(hue, sat, val);
  }

  void assignSnapshotColors(ScreenTimeSnapshot& snapshot) {
    for (auto& app : snapshot.apps) {
      app.chartColor = fixedColorSpec(appSeriesColor(app.appKey));
    }
    for (auto& series : snapshot.chartSeries) {
      series.chartColor = fixedColorSpec(appSeriesColor(series.appKey));
    }
  }

} // namespace

ScreenTimeTab::ScreenTimeTab(ScreenTimeService* screenTime) : m_screenTime(screenTime) {}

std::unique_ptr<Flex> ScreenTimeTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .configure = [](Flex& column) { column.setClipChildren(true); },
  });

  m_rangeDays = 1;
  m_detailDayKey.clear();
  tab->addChild(
      ui::segmented({
          .out = &m_rangePicker,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("control-center.screen-time.range.today")},
                  {.label = i18n::tr("control-center.screen-time.range.3-days")},
                  {.label = i18n::tr("control-center.screen-time.range.14-days")},
              },
          .selectedIndex = 0,
          .fontSize = Style::fontSizeCaption * scale,
          .scale = scale,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t idx) {
            static constexpr int kRanges[] = {1, 3, 14};
            const int nextRange = kRanges[std::min(idx, std::size_t{2})];
            m_detailDayKey.clear();
            m_lastSnapshotKey.clear();
            beginRangeSlideOut(nextRange);
          },
      })
  );

  tab->addChild(
      ui::row(
          {
              .out = &m_detailBar,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Start,
              .gap = Style::spaceSm * scale,
              .fillWidth = true,
              .visible = false,
          },
          ui::button({
              .out = &m_detailBackButton,
              .text = i18n::tr("control-center.screen-time.back"),
              .variant = ButtonVariant::Outline,
              .onClick = [this]() { clearDayDetail(); },
          }),
          ui::label({
              .out = &m_detailTitle,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .flexGrow = 1.0f,
          })
      )
  );

  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .scrollbarVisible = true,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });

  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceLg * scale);

  auto usageCard = ui::column({
      .out = &m_usageCard,
      .gap = Style::spaceMd * scale,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
      },
  });

  usageCard->addChild(
      ui::label({
          .out = &m_disabledLabel,
          .text = i18n::tr("control-center.screen-time.disabled"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );

  usageCard->addChild(
      ui::label({
          .out = &m_totalLabel,
          .fontSize = Style::fontSizeHeader * 1.6f * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );

  auto chartPlotRow = ui::row({
      .out = &m_chartPlotRow,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .gap = kBarGap * scale,
      .minHeight = kChartHeight * scale,
      .maxHeight = kChartHeight * scale,
      .fillWidth = true,
  });

  auto chartLabelRow = ui::row({
      .out = &m_chartLabelRow,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = kBarGap * scale,
      .fillWidth = true,
  });

  for (std::size_t bucketIndex = 0; bucketIndex < m_bucketColumns.size(); ++bucketIndex) {
    auto& bucketColumn = m_bucketColumns[bucketIndex];
    auto plotColumn = ui::column({
        .out = &bucketColumn.plotColumn,
        .align = FlexAlign::Stretch,
        .justify = FlexJustify::End,
        .flexGrow = 1.0f,
        .visible = false,
    });

    plotColumn->addChild(
        ui::box({
            .fill = clearColorSpec(),
            .width = 1.0f,
            .height = kChartHeight * scale,
        })
    );

    plotColumn->addChild(
        ui::box({
            .out = &bucketColumn.track,
            .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
            .participatesInLayout = false,
            .configure = [](Box& box) { box.setZIndex(-1); },
        })
    );

    for (std::size_t series = 0; series < kMaxChartSeries; ++series) {
      auto hitArea = std::make_unique<InputArea>();
      hitArea->setParticipatesInLayout(false);
      hitArea->setVisible(false);
      hitArea->setOnClick([this, bucketIndex](const InputArea::PointerData&) { openDayDetail(bucketIndex); });

      auto segment = ui::box({
          .radius = 0.0f,
          .visible = false,
          .participatesInLayout = false,
      });
      bucketColumn.segments[series] = static_cast<Box*>(hitArea->addChild(std::move(segment)));
      bucketColumn.segmentHits[series] = hitArea.get();
      plotColumn->addChild(std::move(hitArea));
    }

    chartPlotRow->addChild(std::move(plotColumn));

    auto labelCell = ui::row(
        {.out = &bucketColumn.labelCell,
         .align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .flexGrow = 1.0f,
         .visible = false,
         .configure = [scale](Flex& cell) { cell.setMinHeight(Style::fontSizeMini * scale * 2.25f); }},
        ui::label({
            .out = &bucketColumn.label,
            .fontSize = Style::fontSizeMini * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .flexGrow = 1.0f,
            .visible = false,
            .configure = [this, bucketIndex](Label& label) {
              label.setTextAlign(TextAlign::Center);
              const auto requestHoverRedraw = [this]() { PanelManager::instance().requestRedraw(); };
              label.setOnEnter([requestHoverRedraw](const InputArea::PointerData&) { requestHoverRedraw(); });
              label.setOnLeave(requestHoverRedraw);
              label.setOnClick([this, bucketIndex](const InputArea::PointerData&) { openDayDetail(bucketIndex); });
            },
        })
    );
    chartLabelRow->addChild(std::move(labelCell));
  }
  usageCard->addChild(std::move(chartPlotRow));
  usageCard->addChild(std::move(chartLabelRow));
  content->addChild(std::move(usageCard));

  auto mostUsedSection = ui::column({
      .out = &m_mostUsedSection,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  makeSectionHeader(*mostUsedSection, i18n::tr("control-center.screen-time.most-used"), scale);

  auto appsGrid = ui::column({
      .out = &m_appsGrid,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });

  for (std::size_t gridRow = 0; gridRow < m_appGridRows.size(); ++gridRow) {
    auto gridRowFlex = ui::row({
        .out = &m_appGridRows[gridRow],
        .align = FlexAlign::Start,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });

    for (std::size_t col = 0; col < kAppsPerRow; ++col) {
      const std::size_t i = gridRow * kAppsPerRow + col;
      if (i >= m_appRows.size()) {
        break;
      }

      auto cell = ui::column({
          .out = &m_appRows[i].cell,
          .align = FlexAlign::Stretch,
          .fillWidth = true,
          .flexGrow = 1.0f,
          .visible = false,
      });

      auto row = ui::row({
          .out = &m_appRows[i].row,
          .align = FlexAlign::Start,
          .gap = Style::spaceSm * scale,
          .visible = false,
      });

      row->addChild(
          ui::box({
              .out = &m_appRows[i].chartSwatch,
              .radius = kLegendSwatch * scale * 0.5f,
              .width = kLegendSwatch * scale,
              .height = kLegendSwatch * scale,
              .visible = false,
          })
      );

      auto iconSlot = ui::row(
          {.out = &m_appRows[i].iconSlot,
           .align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .width = kAppIconSize * scale,
           .height = kAppIconSize * scale},
          ui::image({
              .out = &m_appRows[i].icon,
              .fit = ImageFit::Cover,
              .radius = Style::scaledRadiusMd(scale),
              .width = kAppIconSize * scale,
              .height = kAppIconSize * scale,
              .visible = false,
              .participatesInLayout = false,
          }),
          ui::glyph({
              .out = &m_appRows[i].iconFallback,
              .glyph = "app-window",
              .glyphSize = kAppIconSize * 0.55f * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .participatesInLayout = false,
          })
      );

      row->addChild(std::move(iconSlot));

      row->addChild(
          ui::column(
              {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
              ui::row(
                  {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
                  ui::label({
                      .out = &m_appRows[i].name,
                      .fontSize = Style::fontSizeBody * scale,
                      .color = colorSpecFromRole(ColorRole::OnSurface),
                      .maxLines = 1,
                      .flexGrow = 1.0f,
                  }),
                  ui::label({
                      .out = &m_appRows[i].duration,
                      .fontSize = usageDurationFontSize(scale),
                      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                      .maxLines = 1,
                      .configure = [](Label& label) { label.setTextAlign(TextAlign::End); },
                  })
              ),
              ui::row(
                  {.out = &m_appRows[i].barHost, .align = FlexAlign::Center, .minHeight = kUsageBarHeight * scale},
                  ui::box({
                      .out = &m_appRows[i].barTrack,
                      .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
                      .radius = Style::scaledRadiusSm(scale),
                      .participatesInLayout = false,
                      .configure = [](Box& box) { box.setZIndex(-1); },
                  }),
                  ui::box({
                      .out = &m_appRows[i].barFill,
                      .radius = Style::scaledRadiusSm(scale),
                      .width = 0.0f,
                      .height = kUsageBarHeight * scale,
                  })
              )
          )
      );
      cell->addChild(std::move(row));
      gridRowFlex->addChild(std::move(cell));
    }

    appsGrid->addChild(std::move(gridRowFlex));
  }

  appsGrid->addChild(
      ui::label({
          .out = &m_emptyLabel,
          .text = i18n::tr("control-center.screen-time.empty"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  mostUsedSection->addChild(std::move(appsGrid));
  content->addChild(std::move(mostUsedSection));
  tab->addChild(std::move(scroll));
  syncEnabledUi();
  m_paletteConn = paletteChanged().connect([this] {
    m_lastSnapshotKey.clear();
    PanelManager::instance().requestUpdateOnly();
  });
  return tab;
}

void ScreenTimeTab::cancelRangeSlide() {
  const bool wasAnimating = m_rangeSlideAnimId != 0;
  if (wasAnimating && m_root != nullptr) {
    AnimationManager* animations = m_root->animationManager();
    if (animations != nullptr) {
      animations->cancel(m_rangeSlideAnimId);
    }
    m_rangeSlideAnimId = 0;
  }
  m_startRangeSlideIn = false;
  if (wasAnimating && m_scroll != nullptr) {
    m_scroll->setPosition(m_scrollBaseX, m_scrollBaseY);
    m_scroll->setOpacity(1.0f);
  }
}

void ScreenTimeTab::applyRangeSlide(float progress, bool slidingIn) {
  if (m_scroll == nullptr || m_root == nullptr) {
    return;
  }

  const float travel = m_root->width();
  if (travel <= 0.0f) {
    return;
  }

  const auto direction = static_cast<float>(m_rangeSlideDirection);
  const float baseX = m_scrollBaseX;
  const float baseY = m_scrollBaseY;
  if (slidingIn) {
    m_scroll->setPosition(baseX + direction * travel * (1.0f - progress), baseY);
    m_scroll->setOpacity(0.7f + 0.3f * progress);
  } else {
    m_scroll->setPosition(baseX - direction * travel * progress, baseY);
    m_scroll->setOpacity(1.0f - 0.3f * progress);
  }
}

void ScreenTimeTab::beginRangeSlideOut(int nextRangeDays) {
  if (nextRangeDays == m_rangeDays && m_detailDayKey.empty()) {
    return;
  }

  m_detailDayKey.clear();
  cancelRangeSlide();

  AnimationManager* animations = m_root != nullptr ? m_root->animationManager() : nullptr;
  if (animations == nullptr || m_scroll == nullptr) {
    m_rangeDays = nextRangeDays;
    PanelManager::instance().refresh();
    return;
  }

  m_scrollBaseX = m_scroll->x();
  m_scrollBaseY = m_scroll->y();
  m_rangeSlideDirection = nextRangeDays > m_rangeDays ? 1 : -1;
  m_pendingRangeDays = nextRangeDays;

  PanelManager::instance().requestFrameTick();
  m_rangeSlideAnimId = animations->animate(
      0.0f, 1.0f, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyRangeSlide(progress, false);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_rangeSlideAnimId = 0;
        m_rangeDays = m_pendingRangeDays;
        m_startRangeSlideIn = true;
        PanelManager::instance().refresh();
      },
      m_root
  );
}

void ScreenTimeTab::beginRangeSlideIn() {
  AnimationManager* animations = m_root != nullptr ? m_root->animationManager() : nullptr;
  if (animations == nullptr || m_scroll == nullptr) {
    return;
  }

  applyRangeSlide(0.0f, true);
  PanelManager::instance().requestFrameTick();
  m_rangeSlideAnimId = animations->animate(
      0.0f, 1.0f, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyRangeSlide(progress, true);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_rangeSlideAnimId = 0;
        if (m_scroll != nullptr) {
          m_scroll->setPosition(m_scrollBaseX, m_scrollBaseY);
          m_scroll->setOpacity(1.0f);
        }
      },
      m_root
  );
}

void ScreenTimeTab::onClose() {
  cancelRangeSlide();
  m_root = nullptr;
  m_scroll = nullptr;
  m_usageCard = nullptr;
  m_detailBar = nullptr;
  m_detailTitle = nullptr;
  m_detailBackButton = nullptr;
  m_disabledLabel = nullptr;
  m_rangePicker = nullptr;
  m_chartPlotRow = nullptr;
  m_chartLabelRow = nullptr;
  m_mostUsedSection = nullptr;
  m_appsGrid = nullptr;
  m_totalLabel = nullptr;
  m_emptyLabel = nullptr;
  m_bucketColumns = {};
  m_appRows = {};
  m_appGridRows = {};
  m_bucketDayKeys.clear();
  m_detailDayKey.clear();
  m_lastSnapshotKey.clear();
  m_rangeDays = 1;
  m_paletteConn = {};
}

void ScreenTimeTab::setActive(bool active) {
  m_active = active;
  if (m_active) {
    m_lastSnapshotKey.clear();
    syncEnabledUi();
  }
}

void ScreenTimeTab::syncEnabledUi() {
  const bool enabled = m_screenTime != nullptr && m_screenTime->enabled();
  const bool inDetail = !m_detailDayKey.empty();
  if (m_disabledLabel != nullptr) {
    m_disabledLabel->setVisible(!enabled);
  }
  const bool showUsage = enabled;
  if (m_rangePicker != nullptr) {
    m_rangePicker->setVisible(showUsage && !inDetail);
  }
  if (m_detailBar != nullptr) {
    m_detailBar->setVisible(showUsage && inDetail);
  }
  if (m_totalLabel != nullptr) {
    m_totalLabel->setVisible(showUsage);
  }
  if (m_chartPlotRow != nullptr) {
    m_chartPlotRow->setVisible(showUsage);
  }
  if (m_chartLabelRow != nullptr) {
    m_chartLabelRow->setVisible(showUsage);
  }
  if (m_mostUsedSection != nullptr) {
    m_mostUsedSection->setVisible(showUsage);
  }
}

void ScreenTimeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }

  const bool slidingOut = m_rangeSlideAnimId != 0 && !m_startRangeSlideIn;
  if (slidingOut) {
    m_root->layout(renderer);
    return;
  }

  syncContent(renderer);
  bindAppNameMaxWidths(renderer, contentWidth);
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
  layoutChart(renderer);
  layoutAppRows(renderer);
  syncDayLabelHover();

  if (m_startRangeSlideIn) {
    m_startRangeSlideIn = false;
    if (m_scroll != nullptr) {
      m_scrollBaseX = m_scroll->x();
      m_scrollBaseY = m_scroll->y();
    }
    beginRangeSlideIn();
  }
}

void ScreenTimeTab::doUpdate(Renderer& renderer) {
  if (!m_active) {
    return;
  }

  const bool slidingOut = m_rangeSlideAnimId != 0 && !m_startRangeSlideIn;
  if (slidingOut) {
    return;
  }

  syncContent(renderer);
  bindAppNameMaxWidths(renderer, m_root != nullptr ? m_root->width() : 0.0f);
  if (m_root != nullptr) {
    m_root->layout(renderer);
  }
  layoutChart(renderer);
  layoutAppRows(renderer);
  syncDayLabelHover();
}

void ScreenTimeTab::bindAppNameMaxWidths(Renderer& renderer, float gridWidth) {
  if (gridWidth <= 0.0f) {
    return;
  }

  const float scale = contentScale();
  const float columnGap = Style::spaceSm * scale;
  const float cellWidth = std::max(1.0f, (gridWidth - columnGap) * 0.5f);
  const float rowGap = Style::spaceSm * scale;
  const float leadingWidth = kLegendSwatch * scale + kAppIconSize * scale + rowGap * 2.0f;
  const float textColumnWidth = std::max(1.0f, cellWidth - leadingWidth);

  float maxDurationWidth = 0.0f;
  for (const auto& widgets : m_appRows) {
    if (widgets.duration == nullptr || widgets.row == nullptr || !widgets.row->visible()) {
      continue;
    }
    widgets.duration->measure(renderer);
    maxDurationWidth = std::max(maxDurationWidth, widgets.duration->width());
  }
  maxDurationWidth = std::ceil(maxDurationWidth);

  for (auto& widgets : m_appRows) {
    if (widgets.name == nullptr || widgets.row == nullptr || !widgets.row->visible()) {
      continue;
    }

    if (widgets.duration != nullptr) {
      if (maxDurationWidth > 0.0f) {
        widgets.duration->setMinWidth(maxDurationWidth);
      } else {
        widgets.duration->setMinWidth(0.0f);
      }
    }

    const float nameMax = std::max(1.0f, textColumnWidth - maxDurationWidth - rowGap);
    widgets.name->setMaxWidth(nameMax);
  }
}

void ScreenTimeTab::syncContent(Renderer& renderer) {
  if (m_screenTime == nullptr || m_totalLabel == nullptr || m_chartPlotRow == nullptr) {
    return;
  }

  syncEnabledUi();
  if (!m_screenTime->enabled()) {
    m_lastSnapshotKey.clear();
    return;
  }

  ScreenTimeSnapshot snapshot =
      !m_detailDayKey.empty() ? m_screenTime->snapshotForDay(m_detailDayKey) : m_screenTime->snapshot(m_rangeDays);
  assignSnapshotColors(snapshot);
  m_bucketDayKeys = snapshot.bucketDayKeys;
  const std::string key = snapshotKey(m_rangeDays, snapshot) + '|' + m_detailDayKey;
  if (key == m_lastSnapshotKey) {
    return;
  }
  m_lastSnapshotKey = key;

  syncEnabledUi();
  if (m_detailTitle != nullptr) {
    m_detailTitle->setText(m_detailDayKey.empty() ? std::string{} : ScreenTimeService::dayDisplayName(m_detailDayKey));
  }

  m_totalLabel->setText(formatDuration(snapshot.total));

  const std::size_t bucketCount = snapshot.buckets.size();
  // Bar scale uses chart series totals only, not full bucket usage.
  std::int64_t peakChartBucketSeconds = 0;
  for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
    std::int64_t chartBucketTotal = 0;
    for (const auto& series : snapshot.chartSeries) {
      if (bucket < series.buckets.size()) {
        chartBucketTotal += series.buckets[bucket].count();
      }
    }
    if (chartBucketTotal <= 0 && bucket < snapshot.buckets.size()) {
      chartBucketTotal = snapshot.buckets[bucket].count();
    }
    peakChartBucketSeconds = std::max(peakChartBucketSeconds, chartBucketTotal);
  }

  const float scale = contentScale();
  const float chartHeight = kChartHeight * scale;
  const float durationFont = usageDurationFontSize(scale);

  for (std::size_t bucket = 0; bucket < m_bucketColumns.size(); ++bucket) {
    auto& columnWidgets = m_bucketColumns[bucket];
    const bool bucketActive = bucket < bucketCount;
    if (columnWidgets.plotColumn != nullptr) {
      columnWidgets.plotColumn->setVisible(bucketActive);
      if (bucketActive) {
        columnWidgets.plotColumn->setFlexGrow(1.0f);
      }
    }
    if (columnWidgets.labelCell != nullptr) {
      columnWidgets.labelCell->setVisible(bucketActive);
      if (bucketActive) {
        columnWidgets.labelCell->setFlexGrow(1.0f);
      }
    }

    if (columnWidgets.label != nullptr) {
      if (!bucketActive) {
        columnWidgets.label->setVisible(false);
        columnWidgets.label->clearTooltip();
      } else if (snapshot.hourlyBuckets) {
        columnWidgets.label->clearTooltip();
        const int hour = static_cast<int>(bucket);
        if (hour == 0) {
          columnWidgets.label->setText(i18n::tr("control-center.screen-time.hour-0"));
          columnWidgets.label->setVisible(true);
        } else if (hour == 6) {
          columnWidgets.label->setText(i18n::tr("control-center.screen-time.hour-6"));
          columnWidgets.label->setVisible(true);
        } else if (hour == 12) {
          columnWidgets.label->setText(i18n::tr("control-center.screen-time.hour-12"));
          columnWidgets.label->setVisible(true);
        } else if (hour == 18) {
          columnWidgets.label->setText(i18n::tr("control-center.screen-time.hour-18"));
          columnWidgets.label->setVisible(true);
        } else {
          columnWidgets.label->setVisible(false);
        }
      } else if (bucket < snapshot.bucketLabels.size()) {
        columnWidgets.label->setText(snapshot.bucketLabels[bucket]);
        columnWidgets.label->setVisible(true);
        const auto dayTotal = bucket < snapshot.buckets.size() ? snapshot.buckets[bucket] : std::chrono::seconds{0};
        auto tip = appUsageTooltip(snapshot.bucketLabels[bucket], dayTotal);
        tip.push_back({.key = i18n::tr("control-center.screen-time.view-day"), .value = {}});
        columnWidgets.label->setTooltip(std::move(tip));
      } else {
        columnWidgets.label->setVisible(false);
        columnWidgets.label->clearTooltip();
      }
      columnWidgets.label->setFontSize(Style::fontSizeMini * scale);
    }

    if (!bucketActive || columnWidgets.plotColumn == nullptr) {
      for (std::size_t seriesIdx = 0; seriesIdx < kMaxChartSeries; ++seriesIdx) {
        if (columnWidgets.segmentHits[seriesIdx] != nullptr) {
          columnWidgets.segmentHits[seriesIdx]->setVisible(false);
          columnWidgets.segmentHits[seriesIdx]->clearTooltip();
        }
        if (columnWidgets.segments[seriesIdx] != nullptr) {
          columnWidgets.segments[seriesIdx]->setVisible(false);
        }
        columnWidgets.segmentHeights[seriesIdx] = 0.0f;
      }
      continue;
    }

    std::chrono::seconds bucketChartTotal{0};
    for (const auto& series : snapshot.chartSeries) {
      if (bucket < series.buckets.size()) {
        bucketChartTotal += series.buckets[bucket];
      }
    }
    if (bucketChartTotal.count() <= 0 && bucket < snapshot.buckets.size()) {
      bucketChartTotal = snapshot.buckets[bucket];
    }
    const float columnHeight = peakChartBucketSeconds > 0
        ? chartHeight * static_cast<float>(bucketChartTotal.count()) / static_cast<float>(peakChartBucketSeconds)
        : 0.0f;

    for (std::size_t seriesIdx = 0; seriesIdx < kMaxChartSeries; ++seriesIdx) {
      Box* segment = columnWidgets.segments[seriesIdx];
      InputArea* hitArea = columnWidgets.segmentHits[seriesIdx];
      if (segment == nullptr) {
        continue;
      }
      if (seriesIdx >= snapshot.chartSeries.size() || bucketChartTotal.count() <= 0) {
        if (hitArea != nullptr) {
          hitArea->setVisible(false);
          hitArea->clearTooltip();
        }
        segment->setVisible(false);
        columnWidgets.segmentHeights[seriesIdx] = 0.0f;
        continue;
      }

      const auto& series = snapshot.chartSeries[seriesIdx];
      const auto seriesSeconds = bucket < series.buckets.size() ? series.buckets[bucket] : std::chrono::seconds{0};
      if (seriesSeconds.count() <= 0) {
        if (hitArea != nullptr) {
          hitArea->setVisible(false);
          hitArea->clearTooltip();
        }
        segment->setVisible(false);
        columnWidgets.segmentHeights[seriesIdx] = 0.0f;
        continue;
      }

      const float segmentHeight =
          columnHeight * static_cast<float>(seriesSeconds.count()) / static_cast<float>(bucketChartTotal.count());
      segment->setFill(series.chartColor);
      segment->setVisible(true);
      columnWidgets.segmentHeights[seriesIdx] = std::max(2.0f, segmentHeight);
      if (hitArea != nullptr) {
        hitArea->setVisible(true);
        hitArea->setTooltip(appUsageTooltip(series.displayName, seriesSeconds));
      }
    }
  }

  const bool hasApps = !snapshot.apps.empty();
  if (m_emptyLabel != nullptr) {
    m_emptyLabel->setVisible(!hasApps);
  }

  const std::chrono::seconds topTotal = hasApps ? snapshot.apps.front().total : std::chrono::seconds{0};

  for (std::size_t i = 0; i < m_appRows.size(); ++i) {
    auto& widgets = m_appRows[i];
    if (widgets.row == nullptr) {
      continue;
    }
    if (i >= snapshot.apps.size()) {
      if (widgets.cell != nullptr) {
        widgets.cell->setVisible(false);
      }
      widgets.row->setVisible(false);
      if (widgets.duration != nullptr) {
        widgets.duration->clearTooltip();
      }
      if (widgets.name != nullptr) {
        widgets.name->clearTooltip();
      }
      continue;
    }

    const auto& app = snapshot.apps[i];
    if (widgets.cell != nullptr) {
      widgets.cell->setVisible(true);
    }
    widgets.row->setVisible(true);
    if (widgets.name != nullptr) {
      widgets.name->setText(app.displayName);
      widgets.name->setTooltip(appUsageTooltip(app.displayName, app.total));
    }
    if (widgets.duration != nullptr) {
      widgets.duration->setFontSize(durationFont);
      widgets.duration->setText(formatDuration(app.total));
      widgets.duration->setTooltip(appUsageTooltip(app.displayName, app.total));
    }
    if (widgets.chartSwatch != nullptr) {
      widgets.chartSwatch->setVisible(true);
      widgets.chartSwatch->setFill(app.chartColor);
    }
    if (widgets.barFill != nullptr) {
      widgets.barFill->setFill(app.chartColor);
      widgets.barFillRatio =
          topTotal.count() > 0 ? static_cast<float>(app.total.count()) / static_cast<float>(topTotal.count()) : 0.0f;
    }

    updateIconForRow(renderer, widgets, app.appKey, scale);
  }

  for (std::size_t gridRow = 0; gridRow < m_appGridRows.size(); ++gridRow) {
    Flex* gridRowFlex = m_appGridRows[gridRow];
    if (gridRowFlex == nullptr) {
      continue;
    }
    bool rowVisible = false;
    for (std::size_t col = 0; col < kAppsPerRow; ++col) {
      const std::size_t i = gridRow * kAppsPerRow + col;
      if (i < m_appRows.size() && m_appRows[i].cell != nullptr && m_appRows[i].cell->visible()) {
        rowVisible = true;
        break;
      }
    }
    gridRowFlex->setVisible(rowVisible);
  }
}

void ScreenTimeTab::layoutChart(Renderer& renderer) {
  if (m_chartPlotRow == nullptr || !m_chartPlotRow->visible()) {
    return;
  }
  const float scale = contentScale();
  const float chartHeight = kChartHeight * scale;
  const float barGap = kBarGap * scale;

  std::size_t activeBuckets = 0;
  for (const auto& columnWidgets : m_bucketColumns) {
    if (columnWidgets.plotColumn != nullptr && columnWidgets.plotColumn->visible()) {
      activeBuckets++;
    }
  }

  float rowWidth = m_chartPlotRow->width();
  if (rowWidth <= 0.0f && m_usageCard != nullptr) {
    rowWidth = m_usageCard->width();
  }
  auto columnWidth = activeBuckets > 0 && rowWidth > 0.0f
      ? (rowWidth - barGap * static_cast<float>(activeBuckets - 1)) / static_cast<float>(activeBuckets)
      : 0.0f;

  if (columnWidth <= 0.0f && activeBuckets > 0 && m_usageCard != nullptr) {
    m_usageCard->layout(renderer);
    rowWidth = m_chartPlotRow->width();
    if (rowWidth <= 0.0f) {
      rowWidth = m_usageCard->width();
    }
    columnWidth = activeBuckets > 0 && rowWidth > 0.0f
        ? (rowWidth - barGap * static_cast<float>(activeBuckets - 1)) / static_cast<float>(activeBuckets)
        : 0.0f;
  }

  if (columnWidth > 0.0f) {
    for (auto& columnWidgets : m_bucketColumns) {
      if (columnWidgets.plotColumn != nullptr && columnWidgets.plotColumn->visible()) {
        columnWidgets.plotColumn->setMinWidth(columnWidth);
        columnWidgets.plotColumn->setSize(columnWidth, chartHeight);
      }
      if (columnWidgets.labelCell != nullptr && columnWidgets.labelCell->visible()) {
        columnWidgets.labelCell->setMinWidth(columnWidth);
      }
    }
  }

  m_chartPlotRow->layout(renderer);
  if (m_chartLabelRow != nullptr) {
    m_chartLabelRow->layout(renderer);
  }
  for (auto& columnWidgets : m_bucketColumns) {
    if (columnWidgets.plotColumn == nullptr || !columnWidgets.plotColumn->visible()) {
      continue;
    }
    columnWidgets.plotColumn->layout(renderer);
    const float resolvedColumnWidth =
        columnWidth > 0.0f ? columnWidth : std::max(1.0f, columnWidgets.plotColumn->width());
    const float plotHeight = std::max(chartHeight, columnWidgets.plotColumn->height());

    if (columnWidgets.track != nullptr) {
      columnWidgets.track->setVisible(true);
      columnWidgets.track->setPosition(0.0f, plotHeight - chartHeight);
      columnWidgets.track->setSize(resolvedColumnWidth, chartHeight);
    }

    float stackTop = plotHeight;
    for (std::size_t seriesIdx = 0; seriesIdx < kMaxChartSeries; ++seriesIdx) {
      Box* segment = columnWidgets.segments[seriesIdx];
      InputArea* hitArea = columnWidgets.segmentHits[seriesIdx];
      const float segmentHeight = columnWidgets.segmentHeights[seriesIdx];
      if (segment == nullptr || !segment->visible() || segmentHeight <= 0.0f) {
        continue;
      }
      stackTop -= segmentHeight;
      if (hitArea != nullptr) {
        hitArea->setPosition(0.0f, stackTop);
        hitArea->setSize(resolvedColumnWidth, segmentHeight);
      }
      segment->setPosition(0.0f, 0.0f);
      segment->setSize(resolvedColumnWidth, segmentHeight);
    }
  }
}

void ScreenTimeTab::syncDayLabelHover() {
  if (m_rangeDays <= 1 || !m_detailDayKey.empty()) {
    return;
  }

  for (auto& columnWidgets : m_bucketColumns) {
    if (columnWidgets.label == nullptr || !columnWidgets.label->visible()) {
      continue;
    }
    columnWidgets.label->setColor(
        columnWidgets.label->hovered() ? colorSpecFromRole(ColorRole::Primary)
                                       : colorSpecFromRole(ColorRole::OnSurfaceVariant)
    );
  }
}

void ScreenTimeTab::openDayDetail(std::size_t bucketIndex) {
  if (!m_detailDayKey.empty() || bucketIndex >= m_bucketDayKeys.size()) {
    return;
  }
  const std::string& dayKey = m_bucketDayKeys[bucketIndex];
  if (dayKey.empty()) {
    return;
  }
  m_detailDayKey = dayKey;
  m_lastSnapshotKey.clear();
  PanelManager::instance().refresh();
}

void ScreenTimeTab::clearDayDetail() {
  if (m_detailDayKey.empty()) {
    return;
  }
  m_detailDayKey.clear();
  m_lastSnapshotKey.clear();
  PanelManager::instance().refresh();
}

void ScreenTimeTab::layoutAppRows(Renderer& renderer) {
  const float scale = contentScale();
  const float iconSize = kAppIconSize * scale;
  for (auto& widgets : m_appRows) {
    if (widgets.row == nullptr || !widgets.row->visible()) {
      continue;
    }
    if (widgets.iconSlot != nullptr) {
      widgets.iconSlot->setSize(iconSize, iconSize);
      if (widgets.icon != nullptr) {
        widgets.icon->setPosition(0.0f, 0.0f);
        widgets.icon->setSize(iconSize, iconSize);
      }
      if (widgets.iconFallback != nullptr) {
        widgets.iconFallback->measure(renderer);
        const float glyphSize = kAppIconSize * 0.55f * scale;
        widgets.iconFallback->setPosition((iconSize - glyphSize) * 0.5f, (iconSize - glyphSize) * 0.5f);
      }
    }
    if (widgets.barHost != nullptr) {
      widgets.barHost->layout(renderer);
      const float barHeight = kUsageBarHeight * scale;
      const float trackWidth = std::max(0.0f, widgets.barHost->width());
      if (widgets.barTrack != nullptr) {
        widgets.barTrack->setSize(trackWidth, barHeight);
      }
      if (widgets.barFill != nullptr && widgets.barFillRatio > 0.0f && trackWidth > 0.0f) {
        const float fillWidth = std::max(2.0f, trackWidth * widgets.barFillRatio);
        widgets.barFill->setSize(fillWidth, barHeight);
      } else if (widgets.barFill != nullptr) {
        widgets.barFill->setSize(0.0f, barHeight);
      }
    }
  }
}

void ScreenTimeTab::updateIconForRow(
    Renderer& renderer, AppRowWidgets& widgets, const std::string& appKey, float scale
) {
  const std::string iconPath = resolveIconPath(appKey);
  if (iconPath == widgets.iconPath) {
    return;
  }
  widgets.iconPath = iconPath;
  const int targetPx = static_cast<int>(std::round(kAppIconSize * scale));
  const bool hasIcon =
      !iconPath.empty() && widgets.icon != nullptr && widgets.icon->setSourceFile(renderer, iconPath, targetPx, true);
  if (widgets.icon != nullptr) {
    widgets.icon->setVisible(hasIcon);
  }
  if (widgets.iconFallback != nullptr) {
    widgets.iconFallback->setVisible(!hasIcon);
  }
}

std::string ScreenTimeTab::resolveIconPath(const std::string& appKey) const {
  if (appKey.empty() || appKey.starts_with("title:")) {
    return {};
  }

  std::string baseKey = appKey;
  if (const auto sep = baseKey.find('\x1f'); sep != std::string::npos) {
    baseKey = baseKey.substr(0, sep);
  }

  if (const auto internal = internal_apps::metadataForAppId(baseKey); internal.has_value()) {
    return internal->iconPath;
  }

  const int targetPx = static_cast<int>(std::round(kAppIconSize * contentScale()));
  std::string iconName;
  if (const auto entry = app_identity::findDesktopEntry(baseKey, desktopEntries());
      entry.has_value() && !entry->icon.empty()) {
    iconName = entry->icon;
  } else {
    iconName = app_identity::resolveRunningDesktopEntry(baseKey, desktopEntries()).icon;
  }
  if (!iconName.empty()) {
    const std::string& resolved = g_iconResolver.resolve(iconName, targetPx);
    if (!resolved.empty()) {
      return resolved;
    }
  }
  const std::string& resolved = g_iconResolver.resolve(baseKey, targetPx);
  return resolved.empty() ? std::string{} : std::string{resolved};
}

void ScreenTimeTab::onPanelCardOpacityChanged(float opacity) {
  if (m_rangePicker != nullptr) {
    m_rangePicker->setSurfaceOpacity(opacity);
  }
}
