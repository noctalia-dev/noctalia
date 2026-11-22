#include "calendar_widget.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <wayland-client-protocol.h>

namespace {
  constexpr float kCalendarWidth = 340.0F;
  constexpr float kEventsWidth = 240.0F;
  constexpr float kWidgetHeight = 390.0F;
  constexpr float kSectionGap = Style::spaceLg;
  constexpr float kGridGap = Style::spaceXs;
  constexpr float kHeaderHeight = Style::controlHeight;
  constexpr float kWeekdayHeight = 20.0F;
  constexpr float kDayCellHeight = 43.0F;
  constexpr float kDayButtonSize = 34.0F;
  constexpr float kDotDiameter = 4.0F;
  constexpr float kWeekColumnWidth = 24.0F;

} // namespace

DesktopCalendar::DesktopCalendar() {
  auto root = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = kSectionGap * m_contentScale,
  });

  auto calendarArea = std::make_unique<InputArea>();
  m_calendarArea = calendarArea.get();
  calendarArea->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps != 0.0F) {
      if (m_onMonthChange) {
        m_onMonthChange(steps > 0.0F ? 1 : -1);
      }
    }
  });

  auto calendarColumn = ui::column({
      .out = &m_calendarColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * m_contentScale,
  });
  calendarColumn->addChild(
      ui::label({
          .out = &m_todayLabel,
          .text = "",
          .fontSize = Style::fontSizeTitle * m_contentScale,
          .fontWeight = FontWeight::Medium,
          .fontFamily = m_fontFamily,
          .color = colorSpecFromRole(ColorRole::Secondary),
          .maxLines = 1,
          .configure = [this](Label& label) {
            label.setHitTestVisible(true);
            label.setOnClick([this](const InputArea::PointerData&) {
              if (m_onToday) {
                m_onToday();
              }
            });
          },
      })
  );

  auto header = ui::row({
      .out = &m_header,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * m_contentScale,
  });
  header->addChild(
      ui::button({
          .out = &m_previousButton,
          .glyph = Style::rtl() ? "chevron-right" : "chevron-left",
          .variant = ButtonVariant::Ghost,
          .onClick = [this] {
            if (m_onMonthChange) {
              m_onMonthChange(-1);
            }
          },
      })
  );
  header->addChild(
      ui::label({
          .out = &m_monthLabel,
          .fontSize = (Style::fontSizeTitle + Style::spaceXs) * m_contentScale,
          .fontWeight = FontWeight::Bold,
          .fontFamily = m_fontFamily,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .textAlign = TextAlign::Center,
          .flexGrow = 1.0F,
          .configure = [this](Label& label) {
            label.setHitTestVisible(true);
            label.setOnClick([this](const InputArea::PointerData&) {
              if (m_onToday) {
                m_onToday();
              }
            });
          },
      })
  );
  header->addChild(
      ui::button({
          .out = &m_nextButton,
          .glyph = Style::rtl() ? "chevron-left" : "chevron-right",
          .variant = ButtonVariant::Ghost,
          .onClick = [this] {
            if (m_onMonthChange) {
              m_onMonthChange(1);
            }
          },
      })
  );
  calendarColumn->addChild(std::move(header));

  auto grid = ui::column({.out = &m_grid, .align = FlexAlign::Stretch, .gap = kGridGap * m_contentScale});
  calendarColumn->addChild(std::move(grid));
  calendarArea->addChild(std::move(calendarColumn));
  root->addChild(std::move(calendarArea));

  auto eventsColumn = ui::column({
      .out = &m_eventsColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * m_contentScale,
      .visible = m_showEvents,
  });
  eventsColumn->addChild(
      ui::label({
          .out = &m_eventsTitle,
          .text = "",
          .fontSize = Style::fontSizeTitle * m_contentScale,
          .fontWeight = FontWeight::Bold,
          .fontFamily = m_fontFamily,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
      })
  );
  eventsColumn->addChild(
      ui::scrollView({
          .out = &m_eventsScroll,
          .contentScale = m_contentScale,
          .fillWidth = true,
          .fillHeight = true,
          .flexGrow = 1.0F,
      })
  );
  root->addChild(std::move(eventsColumn));

  addChild(std::move(root));
}

void DesktopCalendar::setOptions(bool showEvents, bool showWeekNumbers) {
  if (m_showEvents == showEvents && m_showWeekNumbers == showWeekNumbers) {
    return;
  }
  m_showEvents = showEvents;
  m_showWeekNumbers = showWeekNumbers;
  m_eventsColumn->setVisible(showEvents);
  markLayoutDirty();
}

void DesktopCalendar::setContentScale(float scale) {
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

void DesktopCalendar::setText(const std::string& today, const std::string& events) {
  m_todayLabel->setText(today);
  m_eventsTitle->setText(events);
}

void DesktopCalendar::setFontFamily(const std::string& family) {
  m_fontFamily = family;
  for (Label* label : {m_todayLabel, m_monthLabel, m_eventsTitle}) {
    if (label != nullptr) {
      label->setFontFamily(family);
    }
  }
}

void DesktopCalendar::doLayout(Renderer& renderer) {
  if (m_rootLayout == nullptr || m_calendarArea == nullptr || m_calendarColumn == nullptr || m_grid == nullptr) {
    return;
  }

  const float scale = m_contentScale;
  const float calendarWidth = (kCalendarWidth + (m_showWeekNumbers ? kWeekColumnWidth + kGridGap : 0.0F)) * scale;
  const float eventsWidth = kEventsWidth * scale;
  const float height = kWidgetHeight * scale;
  const float totalWidth = calendarWidth + (m_showEvents ? (kSectionGap * scale + eventsWidth) : 0.0F);

  m_rootLayout->setGap(kSectionGap * scale);
  m_rootLayout->setSize(totalWidth, height);
  m_calendarArea->setSize(calendarWidth, height);
  m_calendarColumn->setGap(Style::spaceSm * scale);
  m_calendarColumn->setSize(calendarWidth, height);
  if (m_header != nullptr) {
    m_header->setGap(Style::spaceSm * scale);
    m_header->setSize(calendarWidth, kHeaderHeight * scale);
  }
  for (Button* button : {m_previousButton, m_nextButton}) {
    if (button == nullptr) {
      continue;
    }
    button->setMinWidth(kHeaderHeight * scale);
    button->setMinHeight(kHeaderHeight * scale);
    button->setGlyphSize(Style::fontSizeBody * scale);
    button->setPadding(Style::spaceXs * scale, Style::spaceXs * scale);
    button->setStyleRadius(Style::radiusMd, scale);
  }
  if (m_todayLabel != nullptr) {
    m_todayLabel->setFontSize(Style::fontSizeTitle * scale);
    m_todayLabel->setMaxWidth(calendarWidth);
  }
  if (m_monthLabel != nullptr) {
    m_monthLabel->setFontSize((Style::fontSizeTitle + Style::spaceXs) * scale);
    m_monthLabel->setMaxWidth(std::max(1.0F, calendarWidth - 2.0F * kHeaderHeight * scale));
  }
  if (m_eventsColumn != nullptr) {
    m_eventsColumn->setVisible(m_showEvents);
    m_eventsColumn->setGap(Style::spaceSm * scale);
    m_eventsColumn->setSize(eventsWidth, height);
  }
  if (m_eventsTitle != nullptr) {
    m_eventsTitle->setFontSize(Style::fontSizeTitle * scale);
    m_eventsTitle->setMaxWidth(eventsWidth);
  }
  if (m_eventsScroll != nullptr) {
    m_eventsScroll->setSize(eventsWidth, std::max(1.0F, height - 40.0F * scale));
    m_eventsScroll->setViewportPaddingH(Style::spaceXs * scale);
    m_eventsScroll->setViewportPaddingV(Style::spaceXs * scale);
  }

  if (m_rebuildContent) {
    m_rebuildContent();
  }
  m_rootLayout->layout(renderer);
  setSize(totalWidth, height);
}

DesktopCalendar::MonthMetrics DesktopCalendar::monthMetrics() const {
  const float scale = m_contentScale;
  const float gap = kGridGap * scale;
  const float width = (kCalendarWidth + (m_showWeekNumbers ? kWeekColumnWidth + kGridGap : 0.0F)) * scale;
  const float weekWidth = m_showWeekNumbers ? kWeekColumnWidth * scale : 0.0F;
  const float dayWidth = std::max(1.0F, (width - (m_showWeekNumbers ? weekWidth + gap : 0.0F) - 6.0F * gap) / 7.0F);
  return {
      .width = width,
      .weekdayHeight = kWeekdayHeight * scale,
      .dayCellHeight = kDayCellHeight * scale,
      .dayButtonSize = std::min(kDayButtonSize * scale, dayWidth),
      .gap = gap,
      .dotDiameter = kDotDiameter * scale,
      .dotGap = 2.0F * scale,
      .weekColumnWidth = weekWidth,
      .weekDaysGap = m_showWeekNumbers ? gap : 0.0F
  };
}

void DesktopCalendar::setTodayText(const std::string& today) { m_todayLabel->setText(today); }
