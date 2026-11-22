#include "shell/desktop/widgets/desktop_calendar_widget.h"

#include "calendar/calendar_service.h"
#include "calendar_widget.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "shell/calendar/calendar_view.h"
#include "system/desktop_entry_launch.h"
#include "time/time_format.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <wayland-client-protocol.h>

DesktopCalendarWidget::DesktopCalendarWidget(
    ConfigService* config, CalendarService* calendar, CompositorPlatform* platform, Options options
)
    : m_config(config), m_calendar(calendar), m_platform(platform), m_showEvents(options.showEvents),
      m_showWeekNumbers(options.showWeekNumbers), m_showEventsLocation(options.showEventsLocation) {}

DesktopCalendarWidget::~DesktopCalendarWidget() {
  if (m_calendar != nullptr && m_calendarCallbackId != 0) {
    m_calendar->removeChangeCallback(m_calendarCallbackId);
  }
}

void DesktopCalendarWidget::create() {
  auto widget = std::make_unique<DesktopCalendar>();
  m_widget = widget.get();
  widget->setOptions(m_showEvents, m_showWeekNumbers);
  widget->setText(
      formatLocalTime(m_config != nullptr ? m_config->config().shell.dateFormat.c_str() : "%A, %x"),
      i18n::tr("control-center.calendar.events")
  );
  widget->setOnMonthChange([this](int delta) { changeMonthBy(delta); });
  widget->setOnToday([this] { focusToday(); });
  widget->setContentLayoutCallback([this] {
    rebuildCalendar();
    if (m_showEvents) {
      rebuildEventList();
    }
  });
  setRoot(std::move(widget));
  focusToday();
  if (m_calendar != nullptr) {
    m_calendarCallbackId = m_calendar->addChangeCallback([this] { markDirty(); });
  }
}

bool DesktopCalendarWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "show_events") {
    if (const auto* enabled = std::get_if<bool>(&value)) {
      m_showEvents = *enabled;
      if (m_widget != nullptr) {
        m_widget->setOptions(m_showEvents, m_showWeekNumbers);
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
  if (key == "show_events_location") {
    if (const auto* enabled = std::get_if<bool>(&value)) {
      m_showEventsLocation = *enabled;
      m_dirty = true;
      layout(renderer);
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopCalendarWidget::doLayout(Renderer& renderer) {
  if (m_widget == nullptr) {
    return;
  }
  m_widget->setOptions(m_showEvents, m_showWeekNumbers);
  m_widget->setContentScale(contentScale());
  m_widget->layout(renderer);
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
  if (m_widget != nullptr) {
    m_widget->setTodayText(
        formatLocalTime(m_config != nullptr ? m_config->config().shell.dateFormat.c_str() : "%A, %x")
    );
  }
  if (m_dirty && !isLayingOut()) {
    requestLayout();
  }
}

void DesktopCalendarWidget::onFontFamilyChanged(const std::string& family, Renderer&) {
  if (m_widget != nullptr) {
    m_widget->setFontFamily(family);
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
  if (m_widget == nullptr) {
    return;
  }
  const float scale = contentScale();
  const auto metrics = m_widget->monthMetrics();

  calendar_view::rebuildMonth({
      .grid = m_widget->grid(),
      .monthLabel = m_widget->monthLabel(),
      .snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr,
      .selected = {.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay},
      .monthOffset = m_monthOffset,
      .showWeekNumbers = m_showWeekNumbers,
      .scale = scale,
      .layout =
          {
              .width = metrics.width,
              .weekdayHeight = metrics.weekdayHeight,
              .dayCellHeight = metrics.dayCellHeight,
              .dayButtonSize = metrics.dayButtonSize,
              .gap = metrics.gap,
              .dotDiameter = metrics.dotDiameter,
              .dotGap = metrics.dotGap,
              .weekColumnWidth = metrics.weekColumnWidth,
              .weekDaysGap = metrics.weekDaysGap,
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
  if (m_widget == nullptr || m_config == nullptr) {
    return;
  }
  const auto& calendarConfig = m_config->config().calendar;
  const auto& tabConfig = m_config->config().controlCenter.calendarTab;
  calendar_view::rebuildEventList({
      .scroll = m_widget->eventsScroll(),
      .reserveScrollbarGutter = true,
      .title = &m_widget->eventsTitle(),
      .snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr,
      .selected = {.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay},
      .scale = contentScale(),
      .showEventsLocation = m_showEventsLocation,
      .dateFormat = calendarConfig.eventDateFormat,
      .timeFormat = calendarConfig.eventTimeFormat,
      .fontFamily = m_fontFamily,
      .state = &m_eventListState,
      .platform = m_platform,
      .hidePastEvents = tabConfig.hidePastEvents,
      .pastEventsThresholdMinutes = tabConfig.pastEventsThresholdMinutes,
      .onToggleHiddenPast =
          [this] {
            m_dirty = true;
            requestLayout();
          },
      .requestRedraw = [this] { requestRedraw(); },
  });
}
