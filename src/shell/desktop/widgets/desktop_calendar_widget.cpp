#include "shell/desktop/widgets/desktop_calendar_widget.h"

#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "system/desktop_entry_launch.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/calendar_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
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
  constexpr std::string_view kEventDateFormat = "%A %e %B";
  constexpr std::string_view kEventTimeFormat = "%H:%M";

} // namespace

DesktopCalendarWidget::DesktopCalendarWidget(ConfigService* config, CalendarService* calendar, Options options)
    : m_config(config), m_calendar(calendar), m_showEvents(options.showEvents),
      m_showWeekNumbers(options.showWeekNumbers) {}

DesktopCalendarWidget::~DesktopCalendarWidget() {
  if (m_calendar != nullptr && m_calendarCallbackId != 0) {
    m_calendar->removeChangeCallback(m_calendarCallbackId);
  }
}

void DesktopCalendarWidget::create() {
  auto root = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = kSectionGap * contentScale(),
  });

  auto calendarArea = std::make_unique<InputArea>();
  m_calendarArea = calendarArea.get();
  calendarArea->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps != 0.0F) {
      changeMonthBy(steps > 0.0F ? 1 : -1);
    }
  });

  auto calendarColumn = ui::column({
      .out = &m_calendarColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * contentScale(),
  });
  calendarColumn->addChild(
      ui::label({
          .out = &m_todayLabel,
          .text = formatLocalTime(m_config != nullptr ? m_config->config().shell.dateFormat.c_str() : "%A, %x"),
          .fontSize = Style::fontSizeTitle * contentScale(),
          .fontWeight = FontWeight::Medium,
          .fontFamily = m_fontFamily,
          .color = colorSpecFromRole(ColorRole::Secondary),
          .maxLines = 1,
          .configure = [this](Label& label) {
            label.setHitTestVisible(true);
            label.setOnClick([this](const InputArea::PointerData&) { focusToday(); });
          },
      })
  );

  auto header = ui::row({
      .out = &m_header,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * contentScale(),
  });
  header->addChild(
      ui::button({
          .out = &m_previousButton,
          .glyph = Style::rtl() ? "chevron-right" : "chevron-left",
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { changeMonthBy(-1); },
      })
  );
  header->addChild(
      ui::label({
          .out = &m_monthLabel,
          .fontSize = (Style::fontSizeTitle + Style::spaceXs) * contentScale(),
          .fontWeight = FontWeight::Bold,
          .fontFamily = m_fontFamily,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .textAlign = TextAlign::Center,
          .flexGrow = 1.0F,
          .configure = [this](Label& label) {
            label.setHitTestVisible(true);
            label.setOnClick([this](const InputArea::PointerData&) { focusToday(); });
          },
      })
  );
  header->addChild(
      ui::button({
          .out = &m_nextButton,
          .glyph = Style::rtl() ? "chevron-left" : "chevron-right",
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { changeMonthBy(1); },
      })
  );
  calendarColumn->addChild(std::move(header));

  auto grid = ui::column({.out = &m_grid, .align = FlexAlign::Stretch, .gap = kGridGap * contentScale()});
  calendarColumn->addChild(std::move(grid));
  calendarArea->addChild(std::move(calendarColumn));
  root->addChild(std::move(calendarArea));

  auto eventsColumn = ui::column({
      .out = &m_eventsColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * contentScale(),
      .visible = m_showEvents,
  });
  eventsColumn->addChild(
      ui::label({
          .out = &m_eventsTitle,
          .text = i18n::tr("control-center.calendar.events"),
          .fontSize = Style::fontSizeTitle * contentScale(),
          .fontWeight = FontWeight::Bold,
          .fontFamily = m_fontFamily,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
      })
  );
  eventsColumn->addChild(
      ui::scrollView({
          .out = &m_eventsScroll,
          .fillWidth = true,
          .fillHeight = true,
          .flexGrow = 1.0F,
      })
  );
  root->addChild(std::move(eventsColumn));

  setRoot(std::move(root));
  focusToday();

  if (m_calendar != nullptr) {
    m_calendarCallbackId = m_calendar->addChangeCallback([this]() { markDirty(); });
  }
}

bool DesktopCalendarWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "show_events") {
    if (const auto* enabled = std::get_if<bool>(&value)) {
      m_showEvents = *enabled;
      if (m_eventsColumn != nullptr) {
        m_eventsColumn->setVisible(m_showEvents);
      }
      m_dirty = true;
      layout(renderer);
      return true;
    }
    return false;
  }
  if (key == "show_week_numbers") {
    if (const auto* enabled = std::get_if<bool>(&value)) {
      m_showWeekNumbers = *enabled;
      m_dirty = true;
      layout(renderer);
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopCalendarWidget::doLayout(Renderer& renderer) {
  if (m_rootLayout == nullptr || m_calendarArea == nullptr || m_calendarColumn == nullptr || m_grid == nullptr) {
    return;
  }

  const float scale = contentScale();
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
    button->setRadius(Style::scaledRadiusMd(scale));
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

  rebuildCalendar();
  if (m_showEvents) {
    rebuildEventList();
  }
  m_rootLayout->layout(renderer);
  calendar_view::layoutEventLinkOverlays(m_eventListState);
  m_dirty = false;
}

void DesktopCalendarWidget::doUpdate(Renderer& /*renderer*/) {
  const calendar_view::State state = calendar_view::stateForOffset(m_monthOffset);
  const int todayKey = calendar_view::dateKey(state.current);
  if (todayKey != m_lastTodayKey) {
    m_lastTodayKey = todayKey;
    m_dirty = true;
  }
  if (m_todayLabel != nullptr) {
    m_todayLabel->setText(
        formatLocalTime(m_config != nullptr ? m_config->config().shell.dateFormat.c_str() : "%A, %x")
    );
  }
  if (m_dirty && !isLayingOut()) {
    requestLayout();
  }
}

void DesktopCalendarWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  for (Label* label : {m_todayLabel, m_monthLabel, m_eventsTitle}) {
    if (label != nullptr) {
      label->setFontFamily(family);
    }
  }
}

void DesktopCalendarWidget::changeMonthBy(int delta) {
  if (delta == 0) {
    return;
  }
  m_monthOffset += delta;
  m_dirty = true;
  requestLayout();
}

void DesktopCalendarWidget::focusToday() {
  const calendar_view::State state = calendar_view::stateForOffset(0);
  m_monthOffset = 0;
  m_selectedYear = state.current.year;
  m_selectedMonth = state.current.month;
  m_selectedDay = state.current.day;
  m_lastTodayKey = calendar_view::dateKey(state.current);
  m_dirty = true;
  requestLayout();
}

void DesktopCalendarWidget::markDirty() {
  m_dirty = true;
  requestLayout();
}

void DesktopCalendarWidget::rebuildCalendar() {
  if (m_grid == nullptr || m_monthLabel == nullptr) {
    return;
  }
  const float scale = contentScale();
  const float gap = kGridGap * scale;
  const float calendarWidth = (kCalendarWidth + (m_showWeekNumbers ? kWeekColumnWidth + kGridGap : 0.0F)) * scale;
  const float weekWidth = m_showWeekNumbers ? kWeekColumnWidth * scale : 0.0F;
  const float dayGridWidth = calendarWidth - (m_showWeekNumbers ? weekWidth + gap : 0.0F);
  const float dayColumnWidth = std::max(1.0F, (dayGridWidth - 6.0F * gap) / 7.0F);
  const float buttonSize = std::min(kDayButtonSize * scale, dayColumnWidth);

  calendar_view::rebuildMonth({
      .grid = *m_grid,
      .monthLabel = *m_monthLabel,
      .snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr,
      .selected = {.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay},
      .monthOffset = m_monthOffset,
      .showWeekNumbers = m_showWeekNumbers,
      .scale = scale,
      .layout =
          {
              .width = calendarWidth,
              .weekdayHeight = kWeekdayHeight * scale,
              .dayCellHeight = kDayCellHeight * scale,
              .dayButtonSize = buttonSize,
              .gap = gap,
              .dotDiameter = kDotDiameter * scale,
              .dotGap = 2.0F * scale,
              .weekColumnWidth = weekWidth,
              .weekDaysGap = m_showWeekNumbers ? gap : 0.0F,
          },
      .fontFamily = m_fontFamily,
      .onDateSelected =
          [this](calendar_view::Date date, int monthShift) {
            m_selectedYear = date.year;
            m_selectedMonth = date.month;
            m_selectedDay = date.day;
            m_monthOffset += monthShift;
            m_dirty = true;
            requestLayout();
          },
      .onDateRightClicked =
          [](calendar_view::Date) { (void)desktop_entry_launch::launchDefaultForMimeType("text/calendar"); },
  });
}

void DesktopCalendarWidget::rebuildEventList() {
  if (m_eventsScroll == nullptr) {
    return;
  }
  calendar_view::rebuildEventList({
      .scroll = *m_eventsScroll,
      .reserveScrollbarGutter = true,
      .title = m_eventsTitle,
      .snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr,
      .selected = {.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay},
      .scale = contentScale(),
      .dateFormat = kEventDateFormat,
      .timeFormat = kEventTimeFormat,
      .fontFamily = m_fontFamily,
      .state = &m_eventListState,
      .requestRedraw = [this]() { requestRedraw(); },
  });
}
