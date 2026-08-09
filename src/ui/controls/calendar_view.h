#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class Flex;
class InputArea;
class Label;
class ScrollView;
struct CalendarSnapshot;

namespace calendar_view {

  struct Date {
    int year = 0;
    int month = -1;
    int day = -1;

    [[nodiscard]] bool valid() const noexcept { return year != 0 && month >= 0 && day > 0; }
    bool operator==(const Date&) const = default;
  };

  struct State {
    Date current;
    int displayYear = 0;
    int displayMonth = 0;
    int displayWeekday = 0;
    bool isCurrentMonth = false;
  };

  struct MonthLayout {
    float width = 0.0F;
    float weekdayHeight = 0.0F;
    float dayCellHeight = 0.0F;
    float dayButtonSize = 0.0F;
    float gap = 0.0F;
    float dotDiameter = 0.0F;
    float dotGap = 0.0F;
    float weekColumnWidth = 0.0F;
    float weekLaneInset = 0.0F;
    float weekDividerWidth = 0.0F;
    float weekDaysGap = 0.0F;
  };

  struct MonthBuildOptions {
    Flex& grid;
    Label& monthLabel;
    const CalendarSnapshot* snapshot = nullptr;
    Date selected;
    int monthOffset = 0;
    bool showWeekNumbers = false;
    float scale = 1.0F;
    MonthLayout layout;
    std::string fontFamily;
    std::function<void(Date date, int monthShift)> onDateSelected;
  };
  struct EventLinkOverlay {
    Flex* row = nullptr;
    InputArea* area = nullptr;
  };

  struct EventListState {
    std::vector<EventLinkOverlay> linkOverlays;
  };

  struct EventListBuildOptions {
    ScrollView& scroll;
    bool reserveScrollbarGutter = false;
    Label* title = nullptr;
    const CalendarSnapshot* snapshot = nullptr;
    Date selected;
    float scale = 1.0F;
    std::string_view dateFormat = "%A %e %B";
    std::string_view timeFormat = "%H:%M";
    std::string fontFamily;
    EventListState* state = nullptr;
    std::function<void()> requestRedraw;
  };

  [[nodiscard]] State stateForOffset(int monthOffset);
  [[nodiscard]] int dateKey(Date date) noexcept;
  void rebuildMonth(const MonthBuildOptions& options);
  void rebuildEventList(const EventListBuildOptions& options);
  void layoutEventLinkOverlays(const EventListState& state);

} // namespace calendar_view
