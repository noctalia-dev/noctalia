#pragma once

#include "render/scene/node.h"

#include <functional>
#include <string>

class Button;
class Flex;
class InputArea;
class Label;
class ScrollView;
class DesktopCalendar : public Node {
public:
  struct MonthMetrics {
    float width;
    float weekdayHeight;
    float dayCellHeight;
    float dayButtonSize;
    float gap;
    float dotDiameter;
    float dotGap;
    float weekColumnWidth;
    float weekDaysGap;
  };
  DesktopCalendar();
  MonthMetrics monthMetrics() const;
  void setOptions(bool showEvents, bool showWeekNumbers);
  void setTodayText(const std::string& today);
  void setText(const std::string& today, const std::string& events);
  void setFontFamily(const std::string& family);
  void setContentScale(float scale);
  void setOnMonthChange(std::function<void(int)> callback) { m_onMonthChange = std::move(callback); }
  void setOnToday(std::function<void()> callback) { m_onToday = std::move(callback); }
  void setContentLayoutCallback(std::function<void()> callback) { m_rebuildContent = std::move(callback); }
  Flex& grid() noexcept { return *m_grid; }
  Label& monthLabel() noexcept { return *m_monthLabel; }
  Label& eventsTitle() noexcept { return *m_eventsTitle; }
  ScrollView& eventsScroll() noexcept { return *m_eventsScroll; }

private:
  void doLayout(Renderer& renderer) override;
  bool m_showEvents = true;
  bool m_showWeekNumbers = false;
  float m_contentScale = 1.0F;
  std::string m_fontFamily;
  std::function<void(int)> m_onMonthChange;
  std::function<void()> m_onToday;
  std::function<void()> m_rebuildContent;
  Flex* m_rootLayout = nullptr;
  InputArea* m_calendarArea = nullptr;
  Flex* m_calendarColumn = nullptr;
  Flex* m_header = nullptr;
  Label* m_todayLabel = nullptr;
  Label* m_monthLabel = nullptr;
  Button* m_previousButton = nullptr;
  Button* m_nextButton = nullptr;
  Flex* m_grid = nullptr;
  Flex* m_eventsColumn = nullptr;
  Label* m_eventsTitle = nullptr;
  ScrollView* m_eventsScroll = nullptr;
};
