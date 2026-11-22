#pragma once

#include "shell/calendar/calendar_view.h"
#include "shell/desktop/desktop_widget.h"

#include <cstdint>
#include <limits>
#include <memory>

class DesktopCalendar;
class CalendarService;
class ConfigService;
class CompositorPlatform;
class Renderer;

class DesktopCalendarWidget : public DesktopWidget {
public:
  struct Options {
    bool showEvents = true;
    bool showWeekNumbers = false;
    bool showEventsLocation = false;
  };

  DesktopCalendarWidget(
      ConfigService* config, CalendarService* calendar, CompositorPlatform* platform, Options options
  );
  ~DesktopCalendarWidget() override;

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  void changeMonthBy(int delta);
  void focusToday();
  void rebuildCalendar();
  void rebuildEventList();
  void markDirty();

  ConfigService* m_config = nullptr;
  CalendarService* m_calendar = nullptr;
  CompositorPlatform* m_platform = nullptr;
  bool m_showEvents = true;
  bool m_showWeekNumbers = false;
  bool m_showEventsLocation = false;
  bool m_dirty = true;

  DesktopCalendar* m_widget = nullptr;
  calendar_view::EventListState m_eventListState;

  int m_selectedYear = std::numeric_limits<int>::min();
  int m_selectedMonth = -1;
  int m_selectedDay = -1;
  int m_monthOffset = 0;
  int m_lastTodayKey = -1;
  std::uint64_t m_calendarCallbackId = 0;
};
