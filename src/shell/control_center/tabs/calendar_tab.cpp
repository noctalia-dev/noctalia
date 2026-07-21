#include "shell/control_center/tabs/calendar_tab.h"

#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_tile.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include <wayland-client-protocol.h>

namespace {

  constexpr float kCalendarGridGap = Style::spaceSm;
  constexpr float kCalendarNavButtonSize = Style::controlHeight;
  constexpr float kCalendarWeekdayRowHeight = Style::fontSizeCaption + Style::spaceXs;
  constexpr float kCalendarHeaderHeight = Style::controlHeight;
  constexpr float kCalendarCellSizeMin = Style::controlHeightSm + Style::spaceXs;
  constexpr float kCalendarCellSizeMax = Style::controlHeightLg + Style::spaceXs;
  constexpr float kCalendarDayButtonSizeMax = Style::controlHeightLg;
  constexpr float kCalendarLayoutEpsilon = 0.5f;

  std::string monthName(int month) {
    if (month < 0 || month > 11) {
      return {};
    }
    std::tm tm{};
    tm.tm_mon = month;
    tm.tm_mday = 1;
    return formatStrftime("%B", tm);
  }

  int daysInMonth(int yearValue, int monthValue) {
    const auto lastDay =
        std::chrono::year{yearValue} / std::chrono::month{static_cast<unsigned>(monthValue + 1)} / std::chrono::last;
    return static_cast<int>(static_cast<unsigned>(lastDay.day()));
  }

  struct CalendarBuildState {
    int currentYear = 0;
    int currentMonth = 0;
    int today = 0;
    int displayYear = 0;
    int displayMonth = 0;
    int displayWeekday = 0;
    bool isCurrentMonth = false;
  };

  CalendarBuildState currentCalendarState(int monthOffset) {
    // Use local civil time, not UTC. system_clock::now() floored to days yields
    // the UTC date, which can be off by one relative to the user's wall clock.
    const std::time_t nowTime = std::time(nullptr);
    std::tm localTm{};
    localtime_r(&nowTime, &localTm);

    CalendarBuildState state;
    state.currentYear = localTm.tm_year + 1900;
    state.currentMonth = localTm.tm_mon;
    state.today = localTm.tm_mday;

    const auto currentMonth =
        std::chrono::year{state.currentYear} / std::chrono::month{static_cast<unsigned>(state.currentMonth + 1)};
    const auto displayYmd =
        std::chrono::year_month_day((currentMonth + std::chrono::months(monthOffset)) / std::chrono::day(1));
    const auto displaySys = std::chrono::sys_days(displayYmd);

    state.displayYear = static_cast<int>(static_cast<std::int32_t>(displayYmd.year()));
    state.displayMonth = static_cast<int>(static_cast<unsigned>(displayYmd.month()) - 1);
    state.displayWeekday = static_cast<int>(std::chrono::weekday(displaySys).c_encoding());
    state.isCurrentMonth = state.displayYear == state.currentYear && state.displayMonth == state.currentMonth;
    return state;
  }

  std::string formatShellDate(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.dateFormat.c_str() : "%A, %x";
    return formatLocalTime(format);
  }

  // YYYYMMDD integer key for a local calendar date, for cheap day-range comparisons.
  int localDateKey(std::chrono::system_clock::time_point tp) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&raw, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
  }

  int dateKey(int year, int month0, int day) { return year * 10000 + (month0 + 1) * 100 + day; }

  // First and last (inclusive) local day an event covers. All-day events carry an exclusive end, so
  // the final midnight is pulled back a day.
  std::pair<int, int> eventDayRange(const CalendarEvent& event) {
    const int startKey = localDateKey(event.start);
    std::chrono::system_clock::time_point endTp = event.end;
    if (event.allDay && event.end > event.start) {
      endTp = event.end - std::chrono::hours{24};
    }
    int endKey = localDateKey(endTp);
    endKey = std::max(endKey, startKey);
    return {startKey, endKey};
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

} // namespace

CalendarTab::CalendarTab(ConfigService* config, CalendarService* calendar) : m_config(config), m_calendar(calendar) {}

std::unique_ptr<Flex> CalendarTab::create() {
  const float scale = contentScale();

  if (m_calendar != nullptr && !m_changeCallbackRegistered) {
    m_changeCallbackRegistered = true;
    m_calendar->addChangeCallback([this]() {
      m_eventsDirty = true;
      PanelManager::instance().refresh();
    });
  }

  if (m_config != nullptr) {
    m_showEventsCard = m_config->config().controlCenter.calendarTab.showEventsCard;
  }

  auto tab = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto calendarArea = std::make_unique<InputArea>();
  calendarArea->setFlexGrow(3.0f);
  calendarArea->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps == 0.0f) {
      return;
    }
    changeMonthBy(steps > 0.0f ? 1 : -1);
  });
  m_calendarArea = calendarArea.get();

  auto calendarCard = ui::column({
      .out = &m_card,
      .gap = Style::spaceMd * scale,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        control_center::applySectionCardStyle(card, scale, opacity, borders);
      },
  });

  calendarCard->addChild(
      ui::label({
          .out = &m_todayLabel,
          .text = formatShellDate(m_config),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::Secondary),
          .maxLines = 1,
          .configure = [this](Label& label) {
            label.setHitTestVisible(true);
            label.setOnClick([this](const InputArea::PointerData&) {
              const CalendarBuildState state = currentCalendarState(m_monthOffset);
              const bool focusedOnToday = m_monthOffset == 0
                  && m_selectedYear == state.currentYear
                  && m_selectedMonth == state.currentMonth
                  && m_selectedDay == state.today;
              if (focusedOnToday) {
                return;
              }
              focusToday();
              PanelManager::instance().refresh();
            });
          },
      })
  );

  auto header = ui::row({
      .out = &m_header,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
      .minHeight = kCalendarHeaderHeight * scale,
  });

  auto previousSlot = ui::row(
      {.out = &m_previousSlot, .align = FlexAlign::Center, .justify = FlexJustify::Center},
      ui::button({
          .out = &m_previousButton,
          .glyph = "chevron-left",
          .variant = ButtonVariant::Ghost,
          .minWidth = kCalendarNavButtonSize * scale,
          .minHeight = kCalendarNavButtonSize * scale,
          .onClick = [this]() { changeMonthBy(-1); },
      })
  );
  header->addChild(std::move(previousSlot));

  auto monthWrap = ui::column(
      {.out = &m_monthWrap, .align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0f},
      ui::label({
          .out = &m_monthLabel,
          .fontSize = (Style::fontSizeTitle + Style::spaceXs) * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      })
  );
  header->addChild(std::move(monthWrap));

  auto nextSlot = ui::row(
      {.out = &m_nextSlot, .align = FlexAlign::Center, .justify = FlexJustify::Center},
      ui::button({
          .out = &m_nextButton,
          .glyph = "chevron-right",
          .variant = ButtonVariant::Ghost,
          .minWidth = kCalendarNavButtonSize * scale,
          .minHeight = kCalendarNavButtonSize * scale,
          .onClick = [this]() { changeMonthBy(1); },
      })
  );
  header->addChild(std::move(nextSlot));

  calendarCard->addChild(std::move(header));

  auto gridViewport = ui::column({
      .out = &m_gridViewport,
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
      .flexGrow = 1.0f,
      .configure = [](Flex& viewport) { viewport.setClipChildren(true); },
  });

  auto grid = ui::column({
      .out = &m_grid,
      .align = FlexAlign::Stretch,
      .gap = kCalendarGridGap * scale,
      .configure = [](Flex& layer) { layer.setParticipatesInLayout(false); },
  });
  gridViewport->addChild(std::move(grid));
  calendarCard->addChild(std::move(gridViewport));
  calendarArea->addChild(std::move(calendarCard));
  tab->addChild(std::move(calendarArea));

  auto eventsCard = ui::column(
      {.out = &m_eventsCard,
       .gap = Style::spaceSm * scale,
       .flexGrow = 2.0f,
       .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](
                        Flex& card
                    ) { control_center::applySectionCardStyle(card, scale, opacity, borders); }},
      ui::label({
          .out = &m_eventsTitle,
          .text = i18n::tr("control-center.calendar.events"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      }),
      ui::scrollView({
          .out = &m_eventsScroll,
          .fillWidth = true,
          .fillHeight = true,
          .flexGrow = 1.0f,
      })
  );
  eventsCard->setVisible(m_showEventsCard);

  tab->addChild(std::move(eventsCard));

  return tab;
}

std::unique_ptr<Flex> CalendarTab::createHeaderActions() {
  const float scale = contentScale();
  if (m_config != nullptr) {
    m_showEventsCard = m_config->config().controlCenter.calendarTab.showEventsCard;
  }
  return ui::row(
      {
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
      },
      ui::button({
          .out = &m_toggleEventsCardButton,
          .glyph = m_showEventsCard ? "calendar-event" : "calendar-off",
          .selected = m_showEventsCard,
          .tooltip = i18n::tr("control-center.calendar.toggle-events-card"),
          .onClick = [this]() { toggleEventsCard(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void CalendarTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr || m_card == nullptr || m_calendarArea == nullptr) {
    return;
  }

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  m_card->setSize(m_calendarArea->width(), m_calendarArea->height());
  m_card->layout(renderer);

  const float innerWidth = std::max(0.0f, m_card->width() - (m_card->paddingLeft() + m_card->paddingRight()));
  const float innerHeight = std::max(0.0f, m_card->height() - (m_card->paddingTop() + m_card->paddingBottom()));
  const CalendarBuildState state = currentCalendarState(m_monthOffset);

  // Default the selection to today until the user picks a day.
  if (m_selectedDay < 0) {
    m_selectedYear = state.currentYear;
    m_selectedMonth = state.currentMonth;
    m_selectedDay = state.today;
  }

  const bool sizeChanged = std::abs(innerWidth - m_lastInnerWidth) >= kCalendarLayoutEpsilon
      || std::abs(innerHeight - m_lastInnerHeight) >= kCalendarLayoutEpsilon;
  const bool displayChanged = state.displayYear != m_lastDisplayYear || state.displayMonth != m_lastDisplayMonth;
  const bool todayChanged =
      state.currentYear != m_lastCurrentYear || state.currentMonth != m_lastCurrentMonth || state.today != m_lastToday;

  if (m_monthSlideAnimId != 0 && !m_startMonthSlideIn) {
    m_rootLayout->layout(renderer);
    return;
  }

  if (!sizeChanged && !displayChanged && !todayChanged && !m_eventsDirty && !m_startMonthSlideIn) {
    return;
  }
  m_eventsDirty = false;

  m_lastInnerWidth = innerWidth;
  m_lastInnerHeight = innerHeight;
  m_lastDisplayYear = state.displayYear;
  m_lastDisplayMonth = state.displayMonth;
  m_lastCurrentYear = state.currentYear;
  m_lastCurrentMonth = state.currentMonth;
  m_lastToday = state.today;

  rebuild();
  if (m_grid != nullptr) {
    if (m_monthSlideAnimId == 0 && !m_startMonthSlideIn) {
      m_grid->setPosition(0.0f, 0.0f);
      m_grid->setOpacity(1.0f);
    }
    m_grid->layout(renderer);
  }
  if (m_startMonthSlideIn && m_gridViewport != nullptr) {
    m_startMonthSlideIn = false;
    beginSlideIn();
  }
  m_rootLayout->layout(renderer);
}

void CalendarTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_todayLabel != nullptr) {
    m_todayLabel->setText(formatShellDate(m_config));
  }
}

void CalendarTab::setActive(bool active) {
  if (!active) {
    return;
  }
  focusToday();
}

void CalendarTab::focusToday() {
  cancelMonthSlide();
  const CalendarBuildState state = currentCalendarState(0);
  m_monthOffset = 0;
  m_selectedYear = state.currentYear;
  m_selectedMonth = state.currentMonth;
  m_selectedDay = state.today;
  m_lastDisplayYear = std::numeric_limits<int>::min();
  m_lastDisplayMonth = -1;
  m_eventsDirty = true;
}

void CalendarTab::onClose() {
  cancelMonthSlide();
  m_rootLayout = nullptr;
  m_calendarArea = nullptr;
  m_card = nullptr;
  m_header = nullptr;
  m_previousSlot = nullptr;
  m_nextSlot = nullptr;
  m_monthWrap = nullptr;
  m_todayLabel = nullptr;
  m_monthLabel = nullptr;
  m_previousButton = nullptr;
  m_nextButton = nullptr;
  m_toggleEventsCardButton = nullptr;
  m_gridViewport = nullptr;
  m_grid = nullptr;
  m_eventsCard = nullptr;
  m_eventsTitle = nullptr;
  m_eventsScroll = nullptr;
  m_selectedYear = std::numeric_limits<int>::min();
  m_selectedMonth = -1;
  m_selectedDay = -1;
  focusToday();
  m_eventsDirty = false;
  m_lastInnerWidth = -1.0f;
  m_lastInnerHeight = -1.0f;
  m_lastCurrentYear = std::numeric_limits<int>::min();
  m_lastCurrentMonth = -1;
  m_lastToday = -1;
}

void CalendarTab::changeMonthBy(int delta) {
  if (delta == 0) {
    return;
  }

  cancelMonthSlide();

  AnimationManager* animations = m_gridViewport != nullptr ? m_gridViewport->animationManager() : nullptr;
  if (animations == nullptr || m_grid == nullptr) {
    m_monthOffset += delta;
    m_lastDisplayYear = std::numeric_limits<int>::min();
    m_lastDisplayMonth = -1;
    PanelManager::instance().refresh();
    return;
  }

  beginSlideOut(delta);
}

void CalendarTab::cancelMonthSlide() {
  if (m_monthSlideAnimId != 0 && m_gridViewport != nullptr) {
    if (AnimationManager* animations = m_gridViewport->animationManager(); animations != nullptr) {
      animations->cancel(m_monthSlideAnimId);
    }
    m_monthSlideAnimId = 0;
  }
  m_pendingMonthDelta = 0;
  m_startMonthSlideIn = false;
  if (m_grid != nullptr) {
    m_grid->setPosition(0.0f, 0.0f);
    m_grid->setOpacity(1.0f);
  }
}

void CalendarTab::applyMonthSlide(float progress, bool slidingIn) {
  if (m_grid == nullptr || m_gridViewport == nullptr) {
    return;
  }

  const float travel = m_gridViewport->width();
  if (travel <= 0.0f) {
    return;
  }

  const auto direction = static_cast<float>(m_monthSlideDirection);
  if (slidingIn) {
    m_grid->setPosition(direction * travel * (1.0f - progress), 0.0f);
    m_grid->setOpacity(0.7f + 0.3f * progress);
  } else {
    m_grid->setPosition(-direction * travel * progress, 0.0f);
    m_grid->setOpacity(1.0f - 0.3f * progress);
  }
}

void CalendarTab::beginSlideOut(int delta) {
  AnimationManager* animations = m_gridViewport != nullptr ? m_gridViewport->animationManager() : nullptr;
  if (animations == nullptr || m_grid == nullptr) {
    m_monthOffset += delta;
    m_lastDisplayYear = std::numeric_limits<int>::min();
    m_lastDisplayMonth = -1;
    PanelManager::instance().refresh();
    return;
  }

  m_monthSlideDirection = delta > 0 ? 1 : -1;
  m_pendingMonthDelta = delta;

  PanelManager::instance().requestFrameTick();
  m_monthSlideAnimId = animations->animate(
      0.0f, 1.0f, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyMonthSlide(progress, false);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_monthSlideAnimId = 0;
        m_monthOffset += m_pendingMonthDelta;
        m_pendingMonthDelta = 0;
        m_lastDisplayYear = std::numeric_limits<int>::min();
        m_lastDisplayMonth = -1;
        m_startMonthSlideIn = true;
        PanelManager::instance().refresh();
      },
      m_gridViewport
  );
}

void CalendarTab::beginSlideIn() {
  AnimationManager* animations = m_gridViewport != nullptr ? m_gridViewport->animationManager() : nullptr;
  if (animations == nullptr || m_grid == nullptr) {
    return;
  }

  applyMonthSlide(0.0f, true);
  PanelManager::instance().requestFrameTick();
  m_monthSlideAnimId = animations->animate(
      0.0f, 1.0f, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyMonthSlide(progress, true);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_monthSlideAnimId = 0;
        if (m_grid != nullptr) {
          m_grid->setPosition(0.0f, 0.0f);
          m_grid->setOpacity(1.0f);
        }
      },
      m_gridViewport
  );
}

void CalendarTab::rebuild() {
  uiAssertNotRendering("CalendarTab::rebuild");
  if (m_grid == nullptr || m_monthLabel == nullptr || m_card == nullptr) {
    return;
  }

  while (!m_grid->children().empty()) {
    m_grid->removeChild(m_grid->children().front().get());
  }

  const float scale = contentScale();
  const float innerWidth = std::max(0.0f, m_card->width() - (m_card->paddingLeft() + m_card->paddingRight()));
  const float innerHeight = std::max(0.0f, m_card->height() - (m_card->paddingTop() + m_card->paddingBottom()));
  const float navWidth = kCalendarNavButtonSize * scale * 2.0f + Style::spaceSm * scale * 2.0f;
  const float monthWidth = std::max(0.0f, innerWidth - navWidth);
  const float gridHeightAvailable =
      std::max(0.0f, innerHeight - kCalendarHeaderHeight * scale - kCalendarGridGap * scale);
  const float weekdayHeight = kCalendarWeekdayRowHeight * scale;
  const float dayCellHeight = std::clamp(
      (gridHeightAvailable - weekdayHeight - kCalendarGridGap * scale * 6.0f) / 6.0f, kCalendarCellSizeMin * scale,
      kCalendarCellSizeMax * scale
  );
  const float dayColumnWidth = std::max(0.0f, (innerWidth - kCalendarGridGap * scale * 6.0f) / 7.0f);
  // Reserve a fixed strip under each day number for event indicator dots so all cells stay aligned.
  const float dotDiameter = std::round(5.0f * scale);
  const float dotGap = std::round(2.0f * scale);
  const float dotStripHeight = dotDiameter;
  const float buttonBudget = std::max(0.0f, dayCellHeight - dotStripHeight - dotGap);
  const float dayButtonSize = std::floor(std::min({buttonBudget, dayColumnWidth, kCalendarDayButtonSizeMax * scale}));

  if (m_header != nullptr) {
    m_header->setSize(innerWidth, kCalendarHeaderHeight * scale);
  }
  if (m_previousSlot != nullptr) {
    m_previousSlot->setSize(kCalendarNavButtonSize * scale, kCalendarHeaderHeight * scale);
  }
  if (m_nextSlot != nullptr) {
    m_nextSlot->setSize(kCalendarNavButtonSize * scale, kCalendarHeaderHeight * scale);
  }
  if (m_previousButton != nullptr) {
    m_previousButton->setSize(kCalendarNavButtonSize * scale, kCalendarNavButtonSize * scale);
  }
  if (m_nextButton != nullptr) {
    m_nextButton->setSize(kCalendarNavButtonSize * scale, kCalendarNavButtonSize * scale);
  }
  if (m_monthWrap != nullptr) {
    m_monthWrap->setSize(monthWidth, kCalendarHeaderHeight * scale);
  }

  const CalendarBuildState state = currentCalendarState(m_monthOffset);
  const int year = state.displayYear;
  const int month = state.displayMonth;

  m_monthLabel->setText(monthName(month) + " " + std::to_string(year));
  m_monthLabel->setMaxWidth(monthWidth);
  const bool focusedOnToday = m_monthOffset == 0
      && m_selectedYear == state.currentYear
      && m_selectedMonth == state.currentMonth
      && m_selectedDay == state.today;
  if (m_monthLabel != nullptr) {
    if (focusedOnToday) {
      m_monthLabel->clearTooltip();
    } else {
      m_monthLabel->setTooltip(i18n::tr("control-center.calendar.today"));
    }
  }
  if (m_todayLabel != nullptr) {
    m_todayLabel->setText(formatShellDate(m_config));
    m_todayLabel->setMaxWidth(innerWidth);
  }

  const int firstDayOfWeek = localeFirstDayOfWeek();
  std::array<std::string, 7> weekdays;
  for (int i = 0; i < 7; ++i) {
    std::tm tm{};
    tm.tm_wday = (firstDayOfWeek + i) % 7;
    tm.tm_mday = 1;
    weekdays[static_cast<std::size_t>(i)] = formatStrftime("%a", tm);
  }
  auto weekdayRow = std::make_unique<GridView>();
  weekdayRow->setColumns(weekdays.size());
  weekdayRow->setColumnGap(kCalendarGridGap * scale);
  weekdayRow->setStretchItems(true);
  weekdayRow->setSize(innerWidth, weekdayHeight);
  weekdayRow->setMinCellHeight(weekdayHeight);
  for (std::size_t i = 0; i < weekdays.size(); ++i) {
    auto dayCell = std::make_unique<GridTile>();
    dayCell->setDirection(FlexDirection::Vertical);
    dayCell->setAlign(FlexAlign::Center);
    dayCell->setJustify(FlexJustify::Center);

    const int columnWeekday = (firstDayOfWeek + static_cast<int>(i)) % 7;
    const bool weekend = columnWeekday == 0 || columnWeekday == 6;
    dayCell->addChild(
        ui::label({
            .text = weekdays[i],
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::Medium,
            .color = colorSpecFromRole(weekend ? ColorRole::Secondary : ColorRole::OnSurfaceVariant),
        })
    );

    weekdayRow->addChild(std::move(dayCell));
  }
  m_grid->addChild(std::move(weekdayRow));

  const int firstWeekdayOffset = (state.displayWeekday - firstDayOfWeek + 7) % 7;
  const int previousMonth = month == 0 ? 11 : month - 1;
  const int previousMonthYear = month == 0 ? year - 1 : year;
  const int previousMonthDays = daysInMonth(previousMonthYear, previousMonth);
  const int monthDays = daysInMonth(year, month);
  const int nextMonth = month == 11 ? 0 : month + 1;
  const int nextMonthYear = month == 11 ? year + 1 : year;

  // Indicator dot colors per day of the displayed month (capped at 3 per day).
  std::array<std::vector<ColorSpec>, 32> monthDots;
  if (m_calendar != nullptr) {
    const int monthFirstKey = dateKey(year, month, 1);
    const int monthLastKey = dateKey(year, month, monthDays);
    for (const CalendarEvent& event : m_calendar->snapshot().events) {
      const auto [startKey, endKey] = eventDayRange(event);
      if (endKey < monthFirstKey || startKey > monthLastKey) {
        continue;
      }
      const ColorSpec color = eventColor(event);
      for (int d = 1; d <= monthDays; ++d) {
        const int key = dateKey(year, month, d);
        if (key < startKey || key > endKey) {
          continue;
        }
        auto& dots = monthDots[static_cast<std::size_t>(d)];
        if (dots.size() < 3) {
          dots.push_back(color);
        }
      }
    }
  }

  auto dayGrid = std::make_unique<GridView>();
  dayGrid->setColumns(7);
  dayGrid->setColumnGap(kCalendarGridGap * scale);
  dayGrid->setStretchItems(true);
  dayGrid->setSize(innerWidth, 0.0f);
  dayGrid->setMinCellHeight(dayCellHeight);

  int day = 1;
  int trailingDay = 1;
  for (int index = 0; index < 42; ++index) {
    auto dayTile = std::make_unique<GridTile>();
    dayTile->setDirection(FlexDirection::Vertical);
    dayTile->setAlign(FlexAlign::Center);
    dayTile->setJustify(FlexJustify::Center);
    dayTile->setGap(dotGap);

    auto dayButton = ui::button({
        .text = "",
        .fontSize = Style::fontSizeBody * scale,
        .contentAlign = ButtonContentAlign::Center,
        .variant = ButtonVariant::Ghost,
        .minWidth = dayButtonSize,
        .minHeight = dayButtonSize,
        .padding = 0.0f,
        .radius = Style::scaledRadiusMd(scale),
        .width = dayButtonSize,
        .height = dayButtonSize,
    });

    int cellYear = year;
    int cellMonth = month;
    int cellDay = 0;
    int cellMonthShift = 0;
    bool inMonth = false;

    const auto mutedDayPalette = [] {
      Button::ButtonPalette ghostPalette = Button::defaultPalette(ButtonVariant::Ghost);
      ghostPalette.normal.label = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.75f);
      return ghostPalette;
    }();

    if (index < firstWeekdayOffset) {
      cellDay = previousMonthDays - firstWeekdayOffset + index + 1;
      cellYear = previousMonthYear;
      cellMonth = previousMonth;
      cellMonthShift = -1;
      dayButton->setText(std::to_string(cellDay));
      dayButton->setCustomPalette(mutedDayPalette);
    } else if (day > monthDays) {
      cellDay = trailingDay;
      cellYear = nextMonthYear;
      cellMonth = nextMonth;
      cellMonthShift = 1;
      dayButton->setText(std::to_string(trailingDay));
      dayButton->setCustomPalette(mutedDayPalette);
      ++trailingDay;
    } else {
      cellDay = day;
      inMonth = true;
      const bool selected = m_selectedYear == year && m_selectedMonth == month && m_selectedDay == day;
      const bool isToday = state.isCurrentMonth && day == state.today;
      dayButton->setText(std::to_string(day));
      if (selected) {
        dayButton->setVariant(ButtonVariant::Primary);
      } else {
        dayButton->label()->setFontWeight(FontWeight::Bold);
        dayButton->label()->setColor(colorSpecFromRole(isToday ? ColorRole::Primary : ColorRole::OnSurface));
      }
      ++day;
    }

    auto selectDay = [this, cellYear, cellMonth, cellDay, cellMonthShift]() {
      m_selectedYear = cellYear;
      m_selectedMonth = cellMonth;
      m_selectedDay = cellDay;
      m_eventsDirty = true;
      if (cellMonthShift != 0) {
        changeMonthBy(cellMonthShift);
      } else {
        PanelManager::instance().refresh();
      }
    };

    dayButton->setOnClick(selectDay);

    dayTile->addChild(std::move(dayButton));

    auto dotStrip = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = dotGap});
    dotStrip->setSize(dayButtonSize, dotStripHeight);
    if (inMonth) {
      for (const ColorSpec& color : monthDots[static_cast<std::size_t>(cellDay)]) {
        dotStrip->addChild(
            ui::box({
                .fill = color,
                .radius = dotDiameter * 0.5f,
                .width = dotDiameter,
                .height = dotDiameter,
            })
        );
      }
    }

    // Make the dot strip below the number select the day too, so the whole cell is clickable.
    auto dotArea = std::make_unique<InputArea>();
    dotArea->setSize(dayButtonSize, dotStripHeight);
    dotArea->setOnClick([selectDay](const InputArea::PointerData&) { selectDay(); });
    dotArea->addChild(std::move(dotStrip));
    dayTile->addChild(std::move(dotArea));

    dayGrid->addChild(std::move(dayTile));
  }

  m_grid->addChild(std::move(dayGrid));

  const float gridContentHeight =
      weekdayHeight + kCalendarGridGap * scale + 6.0f * dayCellHeight + 5.0f * kCalendarGridGap * scale;
  if (m_gridViewport != nullptr) {
    m_gridViewport->setSize(innerWidth, gridContentHeight);
  }
  if (m_grid != nullptr) {
    m_grid->setSize(innerWidth, gridContentHeight);
  }

  rebuildEventList(scale);
}

void CalendarTab::rebuildEventList(float scale) {
  if (m_eventsScroll == nullptr) {
    return;
  }

  Flex* content = m_eventsScroll->content();
  if (content == nullptr) {
    return;
  }
  content->setDirection(FlexDirection::Vertical);
  content->setGap(Style::spaceSm * scale);
  // Stretch rows to full content width so each title measures and paints at the same width.
  // Start-aligned rows lay out at their rounded natural width, which can re-wrap a short title
  // during arrange while the row reserved only a single line of height — overflowing the next event.
  content->setAlign(FlexAlign::Stretch);
  while (!content->children().empty()) {
    content->removeChild(content->children().front().get());
  }

  // Selected-day title.
  std::tm selectedTm{};
  selectedTm.tm_year = m_selectedYear - 1900;
  selectedTm.tm_mon = m_selectedMonth;
  selectedTm.tm_mday = m_selectedDay;
  selectedTm.tm_isdst = -1;
  std::mktime(&selectedTm); // normalize tm_wday
  if (m_eventsTitle != nullptr) {
    m_eventsTitle->setText(formatStrftime("%A %e %B", selectedTm));
  }

  const int selectedKey = dateKey(m_selectedYear, m_selectedMonth, m_selectedDay);
  std::vector<const CalendarEvent*> dayEvents;
  if (m_calendar != nullptr) {
    for (const CalendarEvent& event : m_calendar->snapshot().events) {
      const auto [startKey, endKey] = eventDayRange(event);
      if (selectedKey >= startKey && selectedKey <= endKey) {
        dayEvents.push_back(&event);
      }
    }
  }

  // Soft upper bound on the title width so very long titles still wrap. contentViewportWidth() is
  // exactly the width rows are arranged at (insets + conditional scrollbar gutter), so this cap
  // tracks the painted width and never binds early.
  const float dotWidth = Style::spaceXs * scale;
  const float rowGap = Style::spaceSm * scale;
  const float textMaxWidth = std::max(40.0f, m_eventsScroll->contentViewportWidth() - dotWidth - rowGap);

  if (dayEvents.empty()) {
    content->addChild(
        ui::label({
            .text = i18n::tr("control-center.calendar.no-events"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );
    return;
  }

  for (const CalendarEvent* event : dayEvents) {
    std::string timeText;
    if (event->allDay) {
      timeText = i18n::tr("control-center.calendar.all-day");
    } else {
      const std::time_t raw = std::chrono::system_clock::to_time_t(event->start);
      std::tm tm{};
      localtime_r(&raw, &tm);
      timeText = formatStrftime("%H:%M", tm);
    }

    auto dot = ui::box({
        .fill = eventColor(*event),
        .radius = Style::spaceXs * 0.5f * scale,
        .width = dotWidth,
        .flexGrow = 0.0f,
    });

    Label* titleLabel = nullptr;
    Label* timeLabel = nullptr;
    auto details = ui::column(
        {.align = FlexAlign::Start, .gap = Style::spaceXs * 0.5f * scale, .flexGrow = 1.0f},
        ui::label({
            .out = &titleLabel,
            .text = event->title.empty() ? i18n::tr("control-center.calendar.events") : event->title,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 3,
        }),
        ui::label({
            .out = &timeLabel,
            .text = timeText,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );
    if (titleLabel != nullptr) {
      titleLabel->setMaxWidth(textMaxWidth);
    }
    if (timeLabel != nullptr) {
      timeLabel->setMaxWidth(textMaxWidth);
    }

    auto eventRow = ui::row({.align = FlexAlign::Stretch, .gap = rowGap}, std::move(dot), std::move(details));
    content->addChild(std::move(eventRow));
  }
}

void CalendarTab::toggleEventsCard() {
  m_showEventsCard = !m_showEventsCard;
  if (m_config != nullptr) {
    m_config->setOverride({"control_center", "calendar", "show_events_card"}, m_showEventsCard);
  }
  if (m_toggleEventsCardButton != nullptr) {
    m_toggleEventsCardButton->setGlyph(m_showEventsCard ? "calendar-event" : "calendar-off");
    m_toggleEventsCardButton->setSelected(m_showEventsCard);
  }
  if (m_eventsCard != nullptr) {
    m_eventsCard->setVisible(m_showEventsCard);
  }
}
