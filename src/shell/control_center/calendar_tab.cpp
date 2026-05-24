#include "shell/control_center/calendar_tab.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/grid_tile.h"
#include "ui/controls/grid_view.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <memory>
#include <string>
#include <wayland-client-protocol.h>

namespace {

  constexpr float kCalendarGridGap = Style::spaceSm;
  constexpr float kCalendarNavButtonSize = Style::controlHeight;
  constexpr float kCalendarWeekdayRowHeight = Style::controlHeightSm;
  constexpr float kCalendarHeaderHeight = Style::controlHeightLg;
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
    const auto now = std::chrono::system_clock::now();
    const auto today = std::chrono::floor<std::chrono::days>(now);
    const auto ymd = std::chrono::year_month_day(today);

    CalendarBuildState state;
    state.currentYear = static_cast<int>(static_cast<std::int32_t>(ymd.year()));
    state.currentMonth = static_cast<int>(static_cast<unsigned>(ymd.month()) - 1);
    state.today = static_cast<int>(static_cast<unsigned>(ymd.day()));

    auto displayMonth = ymd.month() + std::chrono::months(monthOffset);
    auto displayYmd = std::chrono::year_month_day(ymd.year() / displayMonth / std::chrono::day(1));
    auto displaySys = std::chrono::sys_days(displayYmd);
    displayYmd = std::chrono::year_month_day(displaySys);

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

} // namespace

CalendarTab::CalendarTab(ConfigService* config) : m_config(config) {}

std::unique_ptr<Flex> CalendarTab::create() {
  const float scale = contentScale();

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
    const float delta = data.scrollDelta(1.0f);
    if (delta == 0.0f) {
      return;
    }
    m_scrollAccum += delta;
    if (m_scrollAccum >= 1.0f) {
      m_monthOffset += 1;
      m_scrollAccum -= 1.0f;
      PanelManager::instance().refresh();
    } else if (m_scrollAccum <= -1.0f) {
      m_monthOffset -= 1;
      m_scrollAccum += 1.0f;
      PanelManager::instance().refresh();
    }
  });
  m_calendarArea = calendarArea.get();

  auto calendarCard = ui::column({
      .out = &m_card,
      .gap = Style::spaceMd * scale,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        control_center::applySectionCardStyle(card, scale, opacity, borders);
      },
  });

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
          .onClick = [this]() {
            --m_monthOffset;
            PanelManager::instance().refresh();
          },
      })
  );
  header->addChild(std::move(previousSlot));

  auto monthWrap = ui::column(
      {.out = &m_monthWrap, .align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0f},
      ui::label({
          .out = &m_monthLabel,
          .fontSize = (Style::fontSizeTitle + Style::spaceXs) * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .fontWeight = FontWeight::Bold,
      }),
      ui::label({
          .out = &m_monthSubLabel,
          .text = formatShellDate(m_config),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .configure = [](Label& label) { label.setCaptionStyle(); },
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
          .onClick = [this]() {
            ++m_monthOffset;
            PanelManager::instance().refresh();
          },
      })
  );
  header->addChild(std::move(nextSlot));

  calendarCard->addChild(std::move(header));

  auto grid = ui::column({
      .out = &m_grid,
      .align = FlexAlign::Stretch,
      .gap = kCalendarGridGap * scale,
      .flexGrow = 1.0f,
  });
  calendarCard->addChild(std::move(grid));
  calendarArea->addChild(std::move(calendarCard));
  tab->addChild(std::move(calendarArea));

  auto tasksCard = ui::column(
      {.flexGrow = 2.0f,
       .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](
                        Flex& card
                    ) { control_center::applySectionCardStyle(card, scale, opacity, borders); }},
      ui::label({
          .text = i18n::tr("control-center.calendar.tasks"),
          .fontSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .fontWeight = FontWeight::Bold,
      }),
      ui::label({
          .text = i18n::tr("control-center.calendar.no-tasks"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );

  tab->addChild(std::move(tasksCard));

  return tab;
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

  const bool sizeChanged = std::abs(innerWidth - m_lastInnerWidth) >= kCalendarLayoutEpsilon ||
                           std::abs(innerHeight - m_lastInnerHeight) >= kCalendarLayoutEpsilon;
  const bool displayChanged = state.displayYear != m_lastDisplayYear || state.displayMonth != m_lastDisplayMonth;
  const bool todayChanged =
      state.currentYear != m_lastCurrentYear || state.currentMonth != m_lastCurrentMonth || state.today != m_lastToday;
  if (!sizeChanged && !displayChanged && !todayChanged) {
    return;
  }

  m_lastInnerWidth = innerWidth;
  m_lastInnerHeight = innerHeight;
  m_lastDisplayYear = state.displayYear;
  m_lastDisplayMonth = state.displayMonth;
  m_lastCurrentYear = state.currentYear;
  m_lastCurrentMonth = state.currentMonth;
  m_lastToday = state.today;

  rebuild();
  m_rootLayout->layout(renderer);
}

void CalendarTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_monthSubLabel != nullptr) {
    m_monthSubLabel->setText(formatShellDate(m_config));
  }
}

void CalendarTab::onClose() {
  m_rootLayout = nullptr;
  m_calendarArea = nullptr;
  m_card = nullptr;
  m_header = nullptr;
  m_previousSlot = nullptr;
  m_nextSlot = nullptr;
  m_monthWrap = nullptr;
  m_monthLabel = nullptr;
  m_monthSubLabel = nullptr;
  m_previousButton = nullptr;
  m_nextButton = nullptr;
  m_grid = nullptr;
  m_monthOffset = 0;
  m_scrollAccum = 0.0f;
  m_lastInnerWidth = -1.0f;
  m_lastInnerHeight = -1.0f;
  m_lastDisplayYear = std::numeric_limits<int>::min();
  m_lastDisplayMonth = -1;
  m_lastCurrentYear = std::numeric_limits<int>::min();
  m_lastCurrentMonth = -1;
  m_lastToday = -1;
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
  const float dayButtonSize = std::round(std::min({dayCellHeight, dayColumnWidth, kCalendarDayButtonSizeMax * scale}));

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
  if (m_monthSubLabel != nullptr) {
    m_monthSubLabel->setText(formatShellDate(m_config));
    m_monthSubLabel->setMaxWidth(monthWidth);
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
            .fontSize = (Style::fontSizeCaption + 1.0f) * scale,
            .color = colorSpecFromRole(weekend ? ColorRole::Secondary : ColorRole::OnSurfaceVariant),
            .fontWeight = FontWeight::Bold,
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

  auto dayGrid = std::make_unique<GridView>();
  dayGrid->setColumns(7);
  dayGrid->setColumnGap(kCalendarGridGap * scale);
  dayGrid->setSize(innerWidth, 0.0f);
  dayGrid->setMinCellHeight(dayCellHeight);

  int day = 1;
  int trailingDay = 1;
  for (int index = 0; index < 42; ++index) {
    auto dayTile = std::make_unique<GridTile>();
    dayTile->setDirection(FlexDirection::Vertical);
    dayTile->setAlign(FlexAlign::Center);
    dayTile->setJustify(FlexJustify::Center);

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

    if (index < firstWeekdayOffset) {
      const int leadingDay = previousMonthDays - firstWeekdayOffset + index + 1;
      dayButton->setText(std::to_string(leadingDay));
      dayButton->label()->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.75f));
    } else if (day > monthDays) {
      dayButton->setText(std::to_string(trailingDay));
      dayButton->label()->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.75f));
      ++trailingDay;
    } else {
      dayButton->setText(std::to_string(day));
      if (state.isCurrentMonth && day == state.today) {
        dayButton->setVariant(ButtonVariant::Primary);
      } else {
        dayButton->label()->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
      ++day;
    }

    dayTile->addChild(std::move(dayButton));
    dayGrid->addChild(std::move(dayTile));
  }

  m_grid->addChild(std::move(dayGrid));
}
