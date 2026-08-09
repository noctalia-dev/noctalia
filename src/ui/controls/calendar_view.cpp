#include "ui/controls/calendar_view.h"

#include "calendar/calendar_types.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "render/core/color.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_tile.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

  int daysInMonth(int year, int month) {
    const auto last =
        std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month + 1)} / std::chrono::last;
    return static_cast<int>(static_cast<unsigned>(last.day()));
  }

  std::string monthName(int month) {
    if (month < 0 || month > 11) {
      return {};
    }
    std::tm value{};
    value.tm_mon = month;
    value.tm_mday = 1;
#ifdef __GLIBC__
    return formatStrftime("%OB", value);
#else
    return formatStrftime("%B", value);
#endif
  }

  int localDateKey(std::chrono::system_clock::time_point time) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(time);
    std::tm value{};
    localtime_r(&raw, &value);
    return calendar_view::dateKey({.year = value.tm_year + 1900, .month = value.tm_mon, .day = value.tm_mday});
  }

  std::pair<int, int> eventDayRange(const CalendarEvent& event) {
    const int start = localDateKey(event.start);
    auto endTime = event.end;
    if (event.allDay && event.end > event.start) {
      endTime -= std::chrono::hours{24};
    }
    return {start, std::max(start, localDateKey(endTime))};
  }

  ColorSpec eventColor(const CalendarEvent& event) {
    Color color;
    if (!event.colorHex.empty() && tryParseHexColor(event.colorHex, color)) {
      return fixedColorSpec(color);
    }
    if (const auto role = colorRoleFromToken(event.colorHex); role.has_value()) {
      return colorSpecFromRole(*role);
    }
    return colorSpecFromRole(ColorRole::Primary);
  }

  std::unique_ptr<Flex> spacer(float width, float height) {
    auto result = ui::column({});
    result->setSize(width, height);
    return result;
  }

} // namespace

namespace calendar_view {

  State stateForOffset(int monthOffset) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);

    State state;
    state.current = {.year = local.tm_year + 1900, .month = local.tm_mon, .day = local.tm_mday};

    const auto currentMonth =
        std::chrono::year{state.current.year} / std::chrono::month{static_cast<unsigned>(state.current.month + 1)};
    const auto displayDate =
        std::chrono::year_month_day((currentMonth + std::chrono::months{monthOffset}) / std::chrono::day{1});
    state.displayYear = static_cast<int>(static_cast<std::int32_t>(displayDate.year()));
    state.displayMonth = static_cast<int>(static_cast<unsigned>(displayDate.month())) - 1;
    state.displayWeekday = static_cast<int>(std::chrono::weekday(std::chrono::sys_days{displayDate}).c_encoding());
    state.isCurrentMonth = state.displayYear == state.current.year && state.displayMonth == state.current.month;
    return state;
  }

  int dateKey(Date date) noexcept { return date.year * 10000 + (date.month + 1) * 100 + date.day; }

  void rebuildMonth(const MonthBuildOptions& options) {
    uiAssertNotRendering("calendar_view::rebuildMonth");
    while (!options.grid.children().empty()) {
      options.grid.removeChild(options.grid.children().front().get());
    }

    const State state = stateForOffset(options.monthOffset);
    const int year = state.displayYear;
    const int month = state.displayMonth;
    options.monthLabel.setText(monthName(month) + " " + std::to_string(year));
    const bool focusedOnToday = options.monthOffset == 0 && options.selected == state.current;
    if (focusedOnToday) {
      options.monthLabel.clearTooltip();
    } else {
      options.monthLabel.setTooltip(i18n::tr("control-center.calendar.today"));
    }

    const MonthLayout& layout = options.layout;
    const float weekOverhead = options.showWeekNumbers
        ? layout.weekColumnWidth + layout.weekLaneInset + layout.weekDividerWidth + layout.weekDaysGap
        : 0.0F;
    const float dayGridWidth = std::max(0.0F, layout.width - weekOverhead);

    const int firstDayOfWeek = localeFirstDayOfWeek();
    std::array<std::string, 7> weekdays;
    for (int index = 0; index < 7; ++index) {
      std::tm value{};
      value.tm_wday = (firstDayOfWeek + index) % 7;
      value.tm_mday = 1;
      weekdays[static_cast<std::size_t>(index)] = formatStrftime("%a", value);
    }

    auto weekdayRow = std::make_unique<GridView>();
    weekdayRow->setColumns(weekdays.size());
    weekdayRow->setColumnGap(layout.gap);
    weekdayRow->setStretchItems(true);
    weekdayRow->setSize(dayGridWidth, layout.weekdayHeight);
    weekdayRow->setMinCellHeight(layout.weekdayHeight);
    for (std::size_t index = 0; index < weekdays.size(); ++index) {
      auto tile = std::make_unique<GridTile>();
      tile->setDirection(FlexDirection::Vertical);
      tile->setAlign(FlexAlign::Center);
      tile->setJustify(FlexJustify::Center);
      const int weekday = (firstDayOfWeek + static_cast<int>(index)) % 7;
      tile->addChild(
          ui::label({
              .text = weekdays[index],
              .fontSize = Style::fontSizeCaption * options.scale,
              .fontWeight = FontWeight::Medium,
              .fontFamily = options.fontFamily,
              .color =
                  colorSpecFromRole(weekday == 0 || weekday == 6 ? ColorRole::Secondary : ColorRole::OnSurfaceVariant),
              .maxLines = 1,
          })
      );
      weekdayRow->addChild(std::move(tile));
    }

    const int firstWeekdayOffset = (state.displayWeekday - firstDayOfWeek + 7) % 7;
    const int previousMonth = month == 0 ? 11 : month - 1;
    const int previousYear = month == 0 ? year - 1 : year;
    const int previousDays = daysInMonth(previousYear, previousMonth);
    const int monthDays = daysInMonth(year, month);
    const int nextMonth = month == 11 ? 0 : month + 1;
    const int nextYear = month == 11 ? year + 1 : year;

    std::array<std::vector<ColorSpec>, 32> eventDots;
    if (options.snapshot != nullptr) {
      const int firstKey = dateKey({.year = year, .month = month, .day = 1});
      const int lastKey = dateKey({.year = year, .month = month, .day = monthDays});
      for (const CalendarEvent& event : options.snapshot->events) {
        const auto [eventStart, eventEnd] = eventDayRange(event);
        if (eventEnd < firstKey || eventStart > lastKey) {
          continue;
        }
        const ColorSpec color = eventColor(event);
        for (int day = 1; day <= monthDays; ++day) {
          const int key = dateKey({.year = year, .month = month, .day = day});
          auto& dots = eventDots[static_cast<std::size_t>(day)];
          if (key >= eventStart && key <= eventEnd && dots.size() < 3) {
            dots.push_back(color);
          }
        }
      }
    }

    auto dayGrid = std::make_unique<GridView>();
    dayGrid->setColumns(7);
    dayGrid->setColumnGap(layout.gap);
    dayGrid->setStretchItems(true);
    dayGrid->setSize(dayGridWidth, 6.0F * layout.dayCellHeight + 5.0F * layout.gap);
    dayGrid->setMinCellHeight(layout.dayCellHeight);

    int inMonthDay = 1;
    int trailingDay = 1;
    for (int index = 0; index < 42; ++index) {
      Date date{.year = year, .month = month};
      int monthShift = 0;
      bool inMonth = false;
      if (index < firstWeekdayOffset) {
        date.day = previousDays - firstWeekdayOffset + index + 1;
        date.year = previousYear;
        date.month = previousMonth;
        monthShift = -1;
      } else if (inMonthDay > monthDays) {
        date.day = trailingDay++;
        date.year = nextYear;
        date.month = nextMonth;
        monthShift = 1;
      } else {
        date.day = inMonthDay++;
        inMonth = true;
      }

      auto tile = std::make_unique<GridTile>();
      tile->setDirection(FlexDirection::Vertical);
      tile->setAlign(FlexAlign::Center);
      tile->setJustify(FlexJustify::Center);
      tile->setGap(layout.dotGap);

      auto button = ui::button({
          .text = std::to_string(date.day),
          .fontSize = Style::fontSizeBody * options.scale,
          .contentAlign = ButtonContentAlign::Center,
          .variant = ButtonVariant::Ghost,
          .minWidth = layout.dayButtonSize,
          .minHeight = layout.dayButtonSize,
          .padding = 0.0F,
          .radius = Style::scaledRadiusMd(options.scale),
          .width = layout.dayButtonSize,
          .height = layout.dayButtonSize,
      });
      if (button->label() != nullptr) {
        button->label()->setFontFamily(options.fontFamily);
      }

      if (!inMonth) {
        Button::ButtonPalette muted = Button::defaultPalette(ButtonVariant::Ghost);
        muted.normal.label = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.75F);
        button->setCustomPalette(muted);
      } else if (options.selected == date) {
        button->setVariant(ButtonVariant::Primary);
      } else {
        if (state.isCurrentMonth && date.day == state.current.day) {
          Button::ButtonPalette today = Button::defaultPalette(ButtonVariant::Ghost);
          today.normal.label = colorSpecFromRole(ColorRole::Primary);
          button->setCustomPalette(today);
        }
        if (button->label() != nullptr) {
          button->label()->setFontWeight(FontWeight::Bold);
        }
      }

      const auto selectDate = [callback = options.onDateSelected, date, monthShift]() {
        if (callback) {
          callback(date, monthShift);
        }
      };
      button->setOnClick(selectDate);
      tile->addChild(std::move(button));

      auto dots = ui::row({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = layout.dotGap,
      });
      dots->setSize(layout.dayButtonSize, layout.dotDiameter);
      if (inMonth) {
        for (const ColorSpec& color : eventDots[static_cast<std::size_t>(date.day)]) {
          dots->addChild(
              ui::box({
                  .fill = color,
                  .radius = layout.dotDiameter * 0.5F,
                  .width = layout.dotDiameter,
                  .height = layout.dotDiameter,
              })
          );
        }
      }
      auto dotArea = ui::inputArea({});
      dotArea->setSize(layout.dayButtonSize, layout.dotDiameter);
      dotArea->setOnClick([selectDate](const InputArea::PointerData&) { selectDate(); });
      dotArea->addChild(std::move(dots));
      tile->addChild(std::move(dotArea));
      dayGrid->addChild(std::move(tile));
    }

    const float gridHeight = layout.weekdayHeight + layout.gap + 6.0F * layout.dayCellHeight + 5.0F * layout.gap;
    auto days = ui::column({.gap = layout.gap});
    days->setSize(dayGridWidth, gridHeight);
    days->addChild(std::move(weekdayRow));
    days->addChild(std::move(dayGrid));

    if (options.showWeekNumbers) {
      auto weekColumn = ui::column({.align = FlexAlign::Center, .gap = layout.gap});
      weekColumn->addChild(spacer(layout.weekColumnWidth, layout.weekdayHeight));

      const int thursdayColumn = (4 - firstDayOfWeek + 7) % 7;
      const auto firstThursday =
          std::chrono::sys_days(std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month + 1)} / 1)
          - std::chrono::days{firstWeekdayOffset}
          + std::chrono::days{thursdayColumn};
      for (int row = 0; row < 6; ++row) {
        auto labelBox = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center});
        labelBox->setSize(layout.weekColumnWidth, layout.dayButtonSize);
        labelBox->addChild(
            ui::label({
                .text = std::format("{:%V}", firstThursday + std::chrono::days{row * 7}),
                .fontSize = Style::fontSizeCaption * options.scale,
                .fontFamily = options.fontFamily,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.7F),
                .maxLines = 1,
            })
        );
        auto weekCell = ui::column({
            .align = FlexAlign::Center,
            .justify = FlexJustify::Center,
            .gap = layout.dotGap,
        });
        weekCell->setSize(layout.weekColumnWidth, layout.dayCellHeight);
        weekCell->addChild(std::move(labelBox));
        weekCell->addChild(spacer(layout.weekColumnWidth, layout.dotDiameter));
        weekColumn->addChild(std::move(weekCell));
      }
      weekColumn->setSize(layout.weekColumnWidth, gridHeight);

      auto row = ui::row({.gap = 0.0F});
      row->setSize(layout.width, gridHeight);
      row->addChild(std::move(weekColumn));
      if (layout.weekLaneInset > 0.0F) {
        row->addChild(spacer(layout.weekLaneInset, gridHeight));
      }
      if (layout.weekDividerWidth > 0.0F) {
        row->addChild(
            ui::separator({
                .thickness = layout.weekDividerWidth,
                .orientation = SeparatorOrientation::VerticalRule,
                .width = layout.weekDividerWidth,
                .height = gridHeight,
            })
        );
      }
      if (layout.weekDaysGap > 0.0F) {
        row->addChild(spacer(layout.weekDaysGap, gridHeight));
      }
      row->addChild(std::move(days));
      options.grid.addChild(std::move(row));
    } else {
      options.grid.addChild(std::move(days));
    }
    options.grid.setSize(layout.width, gridHeight);
  }

  void rebuildEventList(const EventListBuildOptions& options) {
    uiAssertNotRendering("calendar_view::rebuildEventList");
    Flex* content = options.scroll.content();
    if (content == nullptr || !options.selected.valid()) {
      return;
    }
    content->setDirection(FlexDirection::Vertical);
    content->setAlign(FlexAlign::Stretch);
    content->setGap(Style::spaceSm * options.scale);
    if (options.state != nullptr) {
      options.state->linkOverlays.clear();
    }
    while (!content->children().empty()) {
      content->removeChild(content->children().front().get());
    }

    std::tm selected{};
    selected.tm_year = options.selected.year - 1900;
    selected.tm_mon = options.selected.month;
    selected.tm_mday = options.selected.day;
    selected.tm_isdst = -1;
    const std::time_t selectedRaw = std::mktime(&selected);
    if (options.title != nullptr) {
      options.title->setText(formatLocalUnixTime(static_cast<std::int64_t>(selectedRaw), options.dateFormat));
    }

    const float dotWidth = Style::spaceXs * options.scale;
    const float rowGap = Style::spaceSm * options.scale;
    const float textMaxWidth =
        std::max(40.0F, options.scroll.contentViewportWidth(options.reserveScrollbarGutter) - dotWidth - rowGap);
    const float linkGlyphSize = Style::fontSizeCaption * options.scale;
    const float linkGlyphGap = Style::spaceXs * options.scale;
    const int selectedKey = dateKey(options.selected);
    bool hasEvents = false;
    if (options.snapshot != nullptr) {
      for (const CalendarEvent& event : options.snapshot->events) {
        const auto [start, end] = eventDayRange(event);
        if (selectedKey < start || selectedKey > end) {
          continue;
        }
        hasEvents = true;
        const bool hasLink = options.state != nullptr && !event.url.empty();
        const float timeMaxWidth =
            hasLink ? std::max(40.0F, textMaxWidth - linkGlyphSize - linkGlyphGap) : textMaxWidth;

        std::string timeText;
        if (event.allDay) {
          timeText = i18n::tr("control-center.calendar.all-day");
        } else {
          const std::time_t raw = std::chrono::system_clock::to_time_t(event.start);
          timeText = formatLocalUnixTime(static_cast<std::int64_t>(raw), options.timeFormat);
        }

        auto time = ui::label({
            .text = timeText,
            .fontSize = Style::fontSizeCaption * options.scale,
            .fontFamily = options.fontFamily,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxWidth = timeMaxWidth,
            .maxLines = 1,
        });
        std::unique_ptr<Node> timeLine = std::move(time);
        if (hasLink) {
          timeLine = ui::row(
              {.align = FlexAlign::Center, .gap = linkGlyphGap}, std::move(timeLine),
              ui::glyph({
                  .glyph = "external-link",
                  .glyphSize = linkGlyphSize,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .flexGrow = 0.0F,
              })
          );
        }

        auto details = ui::column(
            {.align = FlexAlign::Start, .gap = Style::spaceXs * 0.5F * options.scale, .flexGrow = 1.0F},
            ui::label({
                .text = event.title.empty() ? i18n::tr("control-center.calendar.events") : event.title,
                .fontSize = Style::fontSizeBody * options.scale,
                .fontFamily = options.fontFamily,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .maxWidth = textMaxWidth,
                .maxLines = 3,
            }),
            std::move(timeLine)
        );

        Flex* eventRow = nullptr;
        auto eventRowNode = ui::row(
            {.out = &eventRow, .align = FlexAlign::Stretch, .gap = rowGap},
            ui::box({
                .fill = eventColor(event),
                .radius = dotWidth * 0.5F,
                .width = dotWidth,
                .flexGrow = 0.0F,
            }),
            std::move(details)
        );
        if (hasLink && eventRow != nullptr) {
          auto area = ui::inputArea({});
          area->setParticipatesInLayout(false);
          area->setZIndex(1);
          area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
          area->setTooltip(event.url);

          Flex* row = eventRow;
          row->setRadius(Style::radiusSm * options.scale);
          const auto setHovered = [row, requestRedraw = options.requestRedraw](bool hovered) {
            if (hovered) {
              row->setFill(colorSpecFromRole(ColorRole::Hover));
            } else {
              row->clearFill();
            }
            if (requestRedraw) {
              requestRedraw();
            }
          };
          area->setOnEnter([setHovered](const InputArea::PointerData&) { setHovered(true); });
          area->setOnLeave([setHovered]() { setHovered(false); });
          area->setOnClick([url = event.url](const InputArea::PointerData&) { (void)net::openInBrowser(url); });

          options.state->linkOverlays.push_back({.row = row, .area = area.get()});
          eventRow->addChild(std::move(area));
        }
        content->addChild(std::move(eventRowNode));
      }
    }

    if (!hasEvents) {
      content->addChild(
          ui::label({
              .text = i18n::tr("control-center.calendar.no-events"),
              .fontSize = Style::fontSizeBody * options.scale,
              .fontFamily = options.fontFamily,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
          })
      );
    }
  }

  void layoutEventLinkOverlays(const EventListState& state) {
    for (const auto& [row, area] : state.linkOverlays) {
      if (row == nullptr || area == nullptr) {
        continue;
      }
      area->setPosition(0.0F, 0.0F);
      area->setFrameSize(row->width(), row->height());
    }
  }

} // namespace calendar_view
