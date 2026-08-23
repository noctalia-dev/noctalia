#include "shell/control_center/tabs/calendar_tab.h"

#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry_launch.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/calendar_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>
#include <wayland-client-protocol.h>

namespace {

  constexpr float kCalendarGridGap = Style::spaceSm;
  constexpr float kCalendarNavButtonSize = Style::controlHeight;
  constexpr float kCalendarWeekdayRowHeight = Style::fontSizeCaption + Style::spaceXs;
  constexpr float kCalendarHeaderHeight = Style::controlHeight;
  constexpr float kCalendarCellSizeMin = Style::controlHeightSm + Style::spaceXs;
  constexpr float kCalendarCellSizeMax = Style::controlHeightLg + Style::spaceXs;
  constexpr float kCalendarDayButtonSizeMax = Style::controlHeightLg;
  constexpr float kCalendarLayoutEpsilon = 0.5F;
  // Week-number column: a fraction of a day column so it stays visually subordinate, floored at the
  // width of the two digits it holds (as a multiple of the caption font size) so it can never clip.
  constexpr float kCalendarWeekColumnRatio = 0.55F;
  constexpr float kCalendarWeekColumnMinFontScale = 1.45F;

  std::string formatShellDate(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.dateFormat.c_str() : "%A, %x";
    return formatLocalTime(format);
  }
} // namespace

CalendarTab::CalendarTab(ConfigService* config, CalendarService* calendar) : m_config(config), m_calendar(calendar) {}

std::unique_ptr<Flex> CalendarTab::create() {
  const float scale = contentScale();

  if (m_calendar != nullptr && !m_changeCallbackRegistered) {
    m_changeCallbackRegistered = true;
    (void)m_calendar->addChangeCallback([this]() {
      m_eventsDirty = true;
      PanelManager::instance().refresh();
    });
  }

  if (m_config != nullptr) {
    m_showEventsCard = m_config->config().controlCenter.calendarTab.showEventsCard;
    m_showWeekNumbers = m_config->config().controlCenter.calendarTab.showWeekNumbers;
  }

  auto tab = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto calendarArea = ui::inputArea({});
  calendarArea->setFlexGrow(3.0F);
  calendarArea->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps == 0.0F) {
      return;
    }
    changeMonthBy(steps > 0.0F ? 1 : -1);
  });
  m_calendarArea = calendarArea.get();

  auto calendarCard = ui::column({
      .out = &m_card,
      .gap = Style::spaceMd * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        control_center::applySectionCardStyle(card, scale, opacity);
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
              const calendar_view::State state = calendar_view::stateForOffset(m_monthOffset);
              const bool focusedOnToday = m_monthOffset == 0
                  && m_selectedYear == state.current.year
                  && m_selectedMonth == state.current.month
                  && m_selectedDay == state.current.day;
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
          .glyph = Style::rtl() ? "chevron-right" : "chevron-left",
          .variant = ButtonVariant::Ghost,
          .minWidth = kCalendarNavButtonSize * scale,
          .minHeight = kCalendarNavButtonSize * scale,
          .onClick = [this]() { changeMonthBy(-1); },
      })
  );
  header->addChild(std::move(previousSlot));

  auto monthWrap = ui::column(
      {.out = &m_monthWrap, .align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0F},
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
          .glyph = Style::rtl() ? "chevron-left" : "chevron-right",
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
      .gap = 0.0F,
      .flexGrow = 1.0F,
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
       .flexGrow = 2.0F,
       .configure = [scale, opacity = panelCardOpacity()](
                        Flex& card
                    ) { control_center::applySectionCardStyle(card, scale, opacity); }},
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
          .flexGrow = 1.0F,
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

  const float innerWidth = std::max(0.0F, m_card->width() - (m_card->paddingLeft() + m_card->paddingRight()));
  const float innerHeight = std::max(0.0F, m_card->height() - (m_card->paddingTop() + m_card->paddingBottom()));
  const calendar_view::State state = calendar_view::stateForOffset(m_monthOffset);

  // Default the selection to today until the user picks a day.
  if (m_selectedDay < 0) {
    m_selectedYear = state.current.year;
    m_selectedMonth = state.current.month;
    m_selectedDay = state.current.day;
  }

  const bool sizeChanged = std::abs(innerWidth - m_lastInnerWidth) >= kCalendarLayoutEpsilon
      || std::abs(innerHeight - m_lastInnerHeight) >= kCalendarLayoutEpsilon;
  const bool displayChanged = state.displayYear != m_lastDisplayYear || state.displayMonth != m_lastDisplayMonth;
  const bool todayChanged = state.current.year != m_lastCurrentYear
      || state.current.month != m_lastCurrentMonth
      || state.current.day != m_lastToday;

  if (m_monthSlideAnimId != 0 && !m_startMonthSlideIn) {
    m_rootLayout->layout(renderer);
    calendar_view::layoutEventLinkOverlays(m_eventListState);
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
  m_lastCurrentYear = state.current.year;
  m_lastCurrentMonth = state.current.month;
  m_lastToday = state.current.day;

  rebuild();
  if (m_grid != nullptr) {
    if (m_monthSlideAnimId == 0 && !m_startMonthSlideIn) {
      m_grid->setPosition(0.0F, 0.0F);
      m_grid->setOpacity(1.0F);
    }
    m_grid->layout(renderer);
  }
  if (m_startMonthSlideIn && m_gridViewport != nullptr) {
    m_startMonthSlideIn = false;
    beginSlideIn();
  }
  m_rootLayout->layout(renderer);
  calendar_view::layoutEventLinkOverlays(m_eventListState);
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
  const calendar_view::State state = calendar_view::stateForOffset(0);
  m_monthOffset = 0;
  m_selectedYear = state.current.year;
  m_selectedMonth = state.current.month;
  m_selectedDay = state.current.day;
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
  m_eventListState.linkOverlays.clear();
  m_selectedYear = std::numeric_limits<int>::min();
  m_selectedMonth = -1;
  m_selectedDay = -1;
  focusToday();
  m_eventsDirty = false;
  m_lastInnerWidth = -1.0F;
  m_lastInnerHeight = -1.0F;
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
    m_grid->setPosition(0.0F, 0.0F);
    m_grid->setOpacity(1.0F);
  }
}

void CalendarTab::applyMonthSlide(float progress, bool slidingIn) {
  if (m_grid == nullptr || m_gridViewport == nullptr) {
    return;
  }

  const float travel = m_gridViewport->width();
  if (travel <= 0.0F) {
    return;
  }

  const auto direction = static_cast<float>(m_monthSlideDirection);
  if (slidingIn) {
    m_grid->setPosition(direction * travel * (1.0F - progress), 0.0F);
    m_grid->setOpacity(0.7F + 0.3F * progress);
  } else {
    m_grid->setPosition(-direction * travel * progress, 0.0F);
    m_grid->setOpacity(1.0F - 0.3F * progress);
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
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
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

  applyMonthSlide(0.0F, true);
  PanelManager::instance().requestFrameTick();
  m_monthSlideAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyMonthSlide(progress, true);
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_monthSlideAnimId = 0;
        if (m_grid != nullptr) {
          m_grid->setPosition(0.0F, 0.0F);
          m_grid->setOpacity(1.0F);
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

  const float scale = contentScale();
  const float innerWidth = std::max(0.0F, m_card->width() - (m_card->paddingLeft() + m_card->paddingRight()));
  const float innerHeight = std::max(0.0F, m_card->height() - (m_card->paddingTop() + m_card->paddingBottom()));
  const float navWidth = kCalendarNavButtonSize * scale * 2.0F + Style::spaceSm * scale * 2.0F;
  const float monthWidth = std::max(0.0F, innerWidth - navWidth);
  const float gridHeightAvailable =
      std::max(0.0F, innerHeight - kCalendarHeaderHeight * scale - kCalendarGridGap * scale);
  const float weekdayHeight = kCalendarWeekdayRowHeight * scale;
  const float dayCellHeight = std::clamp(
      (gridHeightAvailable - weekdayHeight - kCalendarGridGap * scale * 6.0F) / 6.0F, kCalendarCellSizeMin * scale,
      kCalendarCellSizeMax * scale
  );

  // The week-number column takes its share of the row before the day columns split the rest. It is
  // inset from the divider by one grid gap, which reads as balanced against the card padding on its
  // other side without spending a full card padding's worth of width on a two-digit number.
  const float weekLaneInset = m_showWeekNumbers ? kCalendarGridGap * scale : 0.0F;
  const float weekDividerWidth = m_showWeekNumbers ? std::round(1.0F * scale) : 0.0F;
  const float weekLaneOverhead = m_showWeekNumbers ? weekLaneInset + weekDividerWidth + kCalendarGridGap * scale : 0.0F;
  // Solve for a day column that leaves the week column its fraction, then floor the week column at the
  // width of its text and give the day grid whatever is actually left.
  const float provisionalDayColumn = std::max(
      0.0F,
      (innerWidth - kCalendarGridGap * scale * 6.0F - weekLaneOverhead)
          / (m_showWeekNumbers ? 7.0F + kCalendarWeekColumnRatio : 7.0F)
  );
  const float weekColumnWidth = m_showWeekNumbers
      ? std::max(
            std::round(provisionalDayColumn * kCalendarWeekColumnRatio),
            std::round(Style::fontSizeCaption * scale * kCalendarWeekColumnMinFontScale)
        )
      : 0.0F;
  const float dayGridWidth = std::max(0.0F, innerWidth - weekColumnWidth - weekLaneOverhead);
  const float dayColumnWidth = std::max(0.0F, (dayGridWidth - kCalendarGridGap * scale * 6.0F) / 7.0F);
  // Reserve a fixed strip under each day number for event indicator dots so all cells stay aligned.
  const float dotDiameter = std::round(5.0F * scale);
  const float dotGap = std::round(2.0F * scale);
  const float dotStripHeight = dotDiameter;
  const float buttonBudget = std::max(0.0F, dayCellHeight - dotStripHeight - dotGap);
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
  m_monthLabel->setMaxWidth(monthWidth);
  if (m_todayLabel != nullptr) {
    m_todayLabel->setText(formatShellDate(m_config));
    m_todayLabel->setMaxWidth(innerWidth);
  }

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
              .width = innerWidth,
              .weekdayHeight = weekdayHeight,
              .dayCellHeight = dayCellHeight,
              .dayButtonSize = dayButtonSize,
              .gap = kCalendarGridGap * scale,
              .dotDiameter = dotDiameter,
              .dotGap = dotGap,
              .weekColumnWidth = weekColumnWidth,
              .weekLaneInset = weekLaneInset,
              .weekDividerWidth = weekDividerWidth,
              .weekDaysGap = m_showWeekNumbers ? kCalendarGridGap * scale : 0.0F,
          },
      .onDateSelected =
          [this](calendar_view::Date date, int monthShift) {
            m_selectedYear = date.year;
            m_selectedMonth = date.month;
            m_selectedDay = date.day;
            m_eventsDirty = true;
            if (monthShift != 0) {
              changeMonthBy(monthShift);
            } else {
              PanelManager::instance().refresh();
            }
          },
      .onDateRightClicked =
          [](calendar_view::Date) {
            if (desktop_entry_launch::launchDefaultForMimeType("text/calendar")) {
              PanelManager::instance().closePanel();
            }
          },
  });

  if (m_gridViewport != nullptr) {
    m_gridViewport->setSize(innerWidth, m_grid->height());
  }
  rebuildEventList(scale);
}

void CalendarTab::rebuildEventList(float scale) {
  if (m_eventsScroll == nullptr) {
    return;
  }
  std::string_view eventDateFormat = "%A %e %B";
  std::string_view eventTimeFormat = "%H:%M";
  if (m_config != nullptr) {
    eventDateFormat = m_config->config().controlCenter.calendarTab.eventDateFormat;
    eventTimeFormat = m_config->config().controlCenter.calendarTab.eventTimeFormat;
  }
  calendar_view::rebuildEventList({
      .scroll = *m_eventsScroll,
      .title = m_eventsTitle,
      .snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr,
      .selected = {.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay},
      .scale = scale,
      .dateFormat = eventDateFormat,
      .timeFormat = eventTimeFormat,
      .state = &m_eventListState,
      .requestRedraw = []() { PanelManager::instance().requestRedraw(); },
  });
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
