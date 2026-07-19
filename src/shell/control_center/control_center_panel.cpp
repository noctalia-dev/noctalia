#include "shell/control_center/control_center_panel.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/tabs/screen_time_tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/dependency_service.h"
#include "system/easyeffects_service.h"
#include "system/screen_time_service.h"
#include "ui/builders.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/scroll_view.h"
#include "ui/scroll_into_view.h"
#include "ui/split_pane_focus.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <wayland-client-protocol.h>

using namespace control_center;

namespace {

  constexpr auto kMprisRefreshMinInterval = std::chrono::milliseconds(750);

} // namespace

ControlCenterPanel::ControlCenterPanel(const ControlCenterServices& services) {
  m_hasPowerServices = services.upower != nullptr || services.powerProfiles != nullptr;
  WaylandConnection* wayland = services.platform != nullptr ? &services.platform->wayland() : nullptr;
  m_config = services.config;
  m_mpris = services.mpris;
  m_notificationManager = services.notifications;
  m_dependencies = services.dependencies;
  m_tabs[tabIndex(TabId::Home)] = std::make_unique<HomeTab>(services);
  m_tabs[tabIndex(TabId::Media)] = std::make_unique<MediaTab>(
      services.mpris, services.httpClient, services.spectrum, services.config, wayland,
      PanelManager::instance().renderContext()
  );
  m_tabs[tabIndex(TabId::Audio)] = std::make_unique<AudioTab>(
      services.audio, services.easyEffects, services.mpris, services.config, wayland,
      PanelManager::instance().renderContext()
  );
  m_tabs[tabIndex(TabId::Weather)] = std::make_unique<WeatherTab>(services.weather, services.config);
  m_tabs[tabIndex(TabId::Calendar)] = std::make_unique<CalendarTab>(services.config, services.calendar);
  m_tabs[tabIndex(TabId::Notifications)] = std::make_unique<NotificationsTab>(services.notifications);
  m_tabs[tabIndex(TabId::Network)] =
      std::make_unique<NetworkTab>(services.network, services.networkSecrets, services.externalIp);
  m_tabs[tabIndex(TabId::Bluetooth)] = std::make_unique<BluetoothTab>(services.bluetooth, services.bluetoothAgent);
  m_tabs[tabIndex(TabId::Monitor)] = std::make_unique<MonitorTab>(services.brightness, services.config);
  m_tabs[tabIndex(TabId::System)] = std::make_unique<SystemTab>(services.sysmon);
  m_tabs[tabIndex(TabId::ScreenTime)] = std::make_unique<ScreenTimeTab>(services.screenTime);
  m_tabs[tabIndex(TabId::Power)] = std::make_unique<PowerTab>(services.upower, services.powerProfiles);
  m_tabButtons.fill(nullptr);
  m_tabContainers.fill(nullptr);
  m_tabHeaderActions.fill(nullptr);
}

float ControlCenterPanel::preferredWidth() const {
  const float fullSize = m_config != nullptr ? static_cast<float>(m_config->config().controlCenter.width)
                                             : static_cast<float>(ControlCenterConfig::kDefaultWidth);
  switch (sidebarModeForOpen(pendingOpenContext())) {
  case ControlCenterSidebarMode::Full:
    return fullSize * m_contentScale;
  case ControlCenterSidebarMode::None:
    return fullSize * 0.75f * m_contentScale;
  default:
  case ControlCenterSidebarMode::Compact:
    return fullSize * 0.85f * m_contentScale;
  }
}

PanelPlacement ControlCenterPanel::panelPlacement() const noexcept {
  return m_config == nullptr ? PanelPlacement::Attached : m_config->config().shell.panel.controlCenterPlacement;
}

bool ControlCenterPanel::dismissTransientUi() {
  const std::size_t activeIdx = tabIndex(m_activeTab);
  return m_tabs[activeIdx] != nullptr && m_tabs[activeIdx]->dismissTransientUi();
}

void ControlCenterPanel::create() {
  const float scale = contentScale();
  const ControlCenterSidebarMode sidebarMode = sidebarModeForOpen(pendingOpenContext());
  m_compact = sidebarMode == ControlCenterSidebarMode::Compact;
  m_showSidebar = sidebarMode != ControlCenterSidebarMode::None;

  for (auto& tab : m_tabs) {
    tab->setContentScale(scale);
    tab->setPanelCardOpacity(panelCardOpacity());
    tab->setPanelBordersEnabled(panelBordersEnabled());
  }

  auto rootLayout = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::panelPadding * scale,
      .padding = 0.0f,
  });

  if (m_showSidebar) {
    auto sidebar = ui::column({
        .out = &m_sidebar,
        .align = FlexAlign::Start,
        .gap = 0.0f,
        .padding = Style::spaceSm * scale,
        .fillWidth = false,
        .fillHeight = true,
        .configure = [this, scale](Flex& column) {
          column.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()));
          column.setRadius(Style::scaledRadiusXl(scale));
        },
    });

    auto sidebarScrollArea = std::make_unique<InputArea>();
    sidebarScrollArea->setParticipatesInLayout(false);
    sidebarScrollArea->setZIndex(-1);
    m_sidebarScrollArea = sidebarScrollArea.get();
    wireSidebarScroll(m_sidebarScrollArea);
    sidebar->addChild(std::move(sidebarScrollArea));

    const std::optional<float> sidebarScrollWidth =
        m_compact ? std::optional<float>{Style::controlHeightSm * scale} : std::nullopt;

    auto sidebarScroll = ui::scrollView({
        .out = &m_sidebarScrollView,
        .state = &m_sidebarScrollState,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0f,
        .viewportPaddingV = 0.0f,
        .fillWidth = false,
        .fillHeight = true,
        .width = sidebarScrollWidth,
        .configure = [](ScrollView& scrollView) {
          scrollView.clearFill();
          scrollView.clearBorder();
        },
    });

    auto sidebarNav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::FollowFocus,
        .keepItemsInTabOrder = false,
        .wrap = true,
        .scrollIntoView = [this](const Node* node) { scrollSidebarNodeIntoView(node); },
        .syncIndexFromSelection = {},
    });
    sidebarNav->setTabFocusKey("control-center.sidebar");
    if (!m_compact) {
      sidebarNav->setAlign(FlexAlign::Stretch);
      sidebarNav->setFillWidth(true);
    } else {
      sidebarNav->setAlign(FlexAlign::Start);
    }
    sidebarNav->setGap(Style::spaceXs * scale);
    m_sidebarNav = sidebarNav.get();

    for (const auto& tab : kTabs) {
      const std::size_t idx = tabIndex(tab.id);
      const auto onClick = [this, id = tab.id]() {
        selectTab(id, true);
        PanelManager::instance().refresh();
      };
      sidebarNav->addChild(
          ui::button({
              .out = &m_tabButtons[idx],
              .text = m_compact ? std::optional<std::string>{} : std::optional<std::string>{i18n::tr(tab.titleKey)},
              .glyph = tab.glyph,
              .glyphSize = 21.0f * scale,
              .contentAlign = m_compact ? ButtonContentAlign::Center : ButtonContentAlign::Start,
              .variant = ButtonVariant::Tab,
              .minWidth = m_compact ? std::optional<float>{Style::controlHeightSm * scale} : std::optional<float>{},
              .minHeight = Style::controlHeightSm * scale,
              .paddingV = Style::spaceXs * scale,
              .paddingH = (m_compact ? Style::spaceXs : Style::spaceSm) * scale,
              .gap = Style::spaceSm * scale,
              .radius = Style::scaledRadiusLg(scale),
              .onClick = onClick,
              .configure = [this, scale](Button& button) {
                if (button.label() != nullptr) {
                  button.label()->setFontWeight(FontWeight::Bold);
                  button.label()->setFontSize(Style::fontSizeBody * scale);
                }
                wireSidebarScroll(button.inputArea());
              },
          })
      );
      sidebarNav->registerItem(m_tabButtons[idx], onClick);
    }

    if (sidebarScroll->content() != nullptr) {
      if (!m_compact) {
        sidebarScroll->content()->setAlign(FlexAlign::Stretch);
      }
      sidebarScroll->content()->addChild(std::move(sidebarNav));
    }
    sidebar->addChild(std::move(sidebarScroll));
    rootLayout->addChild(std::move(sidebar));
  }

  auto content = ui::column({
      .out = &m_content,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .clipChildren = true,
      .flexGrow = 4.0f,
  });

  auto dismissArea = std::make_unique<InputArea>();
  dismissArea->setParticipatesInLayout(false);
  dismissArea->setZIndex(-1);
  dismissArea->setFocusable(false);
  dismissArea->setTabStop(false);
  dismissArea->setOnPress([this](const InputArea::PointerData&) {
    const std::size_t activeIdx = tabIndex(m_activeTab);
    if (m_tabs[activeIdx] != nullptr && m_tabs[activeIdx]->dismissTransientUi()) {
      PanelManager::instance().refresh();
    }
  });
  m_contentDismissArea = static_cast<InputArea*>(content->addChild(std::move(dismissArea)));

  auto header = ui::row({
      .out = &m_contentHeader,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
  });

  header->addChild(
      ui::label({
          .out = &m_contentTitle,
          .text = i18n::tr("control-center.tabs.home"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
          .flexGrow = 1.0f,
      })
  );

  auto headerActions = ui::row({
      .out = &m_contentHeaderActions,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });

  for (std::size_t i = 0; i < kTabCount; ++i) {
    auto actions = m_tabs[i]->createHeaderActions();
    m_tabHeaderActions[i] = actions.get();
    if (actions != nullptr) {
      actions->setVisible(false);
      m_contentHeaderActions->addChild(std::move(actions));
    }
  }

  m_contentHeaderActions->addChild(
      ui::button({
          .out = &m_closeButton,
          .glyph = "close",
          .onClick = []() { PanelManager::instance().close(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
  header->addChild(std::move(headerActions));

  content->addChild(std::move(header));

  auto bodies = ui::column({
      .out = &m_tabBodies,
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
      .clipChildren = true,
      .flexGrow = 1.0f,
  });

  for (std::size_t i = 0; i < kTabCount; ++i) {
    auto container = m_tabs[i]->create();
    container->setFlexGrow(1.0f);
    container->setParticipatesInLayout(false);
    container->setVisible(false);
    m_tabContainers[i] = container.get();
    m_tabBodies->addChild(std::move(container));
  }

  content->addChild(std::move(bodies));
  rootLayout->addChild(std::move(content));
  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  syncTabVisibility();
  m_firstOpenAfterCreate = true;
  selectTab(m_activeTab);
}

void ControlCenterPanel::onPanelBordersChanged(bool enabled) {
  for (auto& tab : m_tabs) {
    if (tab != nullptr) {
      tab->setPanelBordersEnabled(enabled);
    }
  }
}

void ControlCenterPanel::onPanelCardOpacityChanged(float opacity) {
  for (auto& tab : m_tabs) {
    if (tab != nullptr) {
      tab->setPanelCardOpacity(opacity);
    }
  }
  if (m_sidebar != nullptr) {
    m_sidebar->setFill(colorSpecFromRole(ColorRole::SurfaceVariant, opacity));
  }
}

void ControlCenterPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_content == nullptr || m_tabBodies == nullptr) {
    return;
  }

  if (!m_compact && m_showSidebar) {
    layoutFullSidebarWidth(renderer);
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  const float contentInnerWidth =
      std::max(0.0f, m_content->width() - (m_content->paddingLeft() + m_content->paddingRight()));
  const float bodyWidth = m_tabBodies->width();
  const float bodyHeight = m_tabBodies->height();

  if (m_sidebarScrollArea != nullptr && m_sidebar != nullptr) {
    m_sidebarScrollArea->setPosition(0.0f, 0.0f);
    m_sidebarScrollArea->setSize(m_sidebar->width(), m_sidebar->height());
  }

  if (m_contentDismissArea != nullptr) {
    m_contentDismissArea->setPosition(0.0f, 0.0f);
    m_contentDismissArea->setFrameSize(m_content->width(), m_content->height());
  }

  if (m_contentHeader != nullptr) {
    m_contentHeader->setSize(contentInnerWidth, 0.0f);
  }

  if (m_contentTitle != nullptr) {
    const float actionsWidth = m_contentHeaderActions != nullptr ? m_contentHeaderActions->width() : 0.0f;
    const float headerGap = m_contentHeader != nullptr ? m_contentHeader->gap() : 0.0f;
    const float titleWidth = std::max(0.0f, contentInnerWidth - actionsWidth - headerGap);
    m_contentTitle->setMaxWidth(titleWidth);
  }

  for (auto* container : m_tabContainers) {
    if (container != nullptr && container->visible()) {
      container->setSize(bodyWidth, bodyHeight);
    }
  }

  layoutTabContainers(bodyWidth, bodyHeight);

  const auto layoutTab = [this, bodyWidth, bodyHeight, &renderer](TabId tabId) {
    const std::size_t idx = tabIndex(tabId);
    if (m_tabs[idx] == nullptr || m_tabContainers[idx] == nullptr || !m_tabContainers[idx]->visible()) {
      return;
    }
    m_tabs[idx]->layout(renderer, bodyWidth, bodyHeight);
  };

  if (m_tabTransitionActive) {
    layoutTab(m_tabTransitionOutgoing);
  }
  layoutTab(m_activeTab);
}

void ControlCenterPanel::doUpdate(Renderer& renderer) {
  if (!isTabVisible(m_activeTab)) {
    selectTab(firstVisibleTab());
  } else {
    syncTabVisibility();
  }
  const std::size_t activeIdx = tabIndex(m_activeTab);
  if (m_tabs[activeIdx] != nullptr) {
    m_tabs[activeIdx]->update(renderer);
  }
}

void ControlCenterPanel::onFrameTick(float deltaMs) {
  const std::size_t activeIdx = tabIndex(m_activeTab);
  if (m_tabs[activeIdx] != nullptr) {
    m_tabs[activeIdx]->onFrameTick(deltaMs);
  }
}

void ControlCenterPanel::onOpen(std::string_view context) {
  if (m_dependencies != nullptr) {
    m_dependencies->rescan();
  }
  const bool animateTabSwitch = !m_firstOpenAfterCreate;
  m_firstOpenAfterCreate = false;
  selectTab(tabFromContext(context), animateTabSwitch);
}

bool ControlCenterPanel::isContextActive(std::string_view context) const {
  return m_activeTab == tabFromContext(context);
}

bool ControlCenterPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!m_showSidebar || m_sidebarNav == nullptr || m_sidebarScrollView == nullptr || m_content == nullptr) {
    return false;
  }

  const SplitPaneFocusConfig panes{
      .sidebarFocus = m_sidebarNav->focusArea(),
      .sidebarRoot = m_sidebarScrollView,
      .contentRoot = m_content,
      .headerFocus = nullptr,
  };
  auto& dispatcher = PanelManager::instance().inputDispatcher();
  const SplitPaneFocusResult splitResult =
      handleSplitPaneFocusNavigation(dispatcher, panes, sym, modifiers, pressed, preedit);
  return splitResult == SplitPaneFocusResult::Consumed;
}

void ControlCenterPanel::onClose() {
  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
  }
  m_tabTransitionActive = false;
  m_activeTab = TabId::Home;
  for (auto& tab : m_tabs) {
    tab->setActive(false);
    tab->onClose();
  }
  m_rootLayout = nullptr;
  m_sidebar = nullptr;
  m_sidebarScrollView = nullptr;
  m_sidebarScrollState = {};
  m_sidebarNav = nullptr;
  m_sidebarScrollArea = nullptr;
  m_content = nullptr;
  m_contentDismissArea = nullptr;
  m_contentHeader = nullptr;
  m_contentHeaderActions = nullptr;
  m_contentTitle = nullptr;
  m_closeButton = nullptr;
  m_tabBodies = nullptr;
  m_tabButtons.fill(nullptr);
  m_tabContainers.fill(nullptr);
  m_tabHeaderActions.fill(nullptr);
  clearReleasedRoot();
}

bool ControlCenterPanel::deferExternalRefresh() const {
  if (m_activeTab != TabId::Audio) {
    return false;
  }
  const auto* audioTab = dynamic_cast<const AudioTab*>(m_tabs[tabIndex(TabId::Audio)].get());
  return audioTab != nullptr && audioTab->dragging();
}

bool ControlCenterPanel::deferPointerRelayout() const { return deferExternalRefresh(); }

bool ControlCenterPanel::isTabFeatureAvailable(TabId tab) const {
  if (m_config == nullptr) {
    switch (tab) {
    case TabId::ScreenTime:
      return false;
    case TabId::Power:
      return m_hasPowerServices;
    default:
      return true;
    }
  }
  const auto& cfg = m_config->config();
  switch (tab) {
  case TabId::Weather:
    return cfg.weather.enabled;
  case TabId::ScreenTime:
    return cfg.shell.screenTimeEnabled;
  case TabId::System:
    return cfg.system.monitor.enabled;
  case TabId::Power:
    return m_hasPowerServices;
  default:
    return true;
  }
}

bool ControlCenterPanel::isTabVisible(TabId tab) const {
  if (!isTabFeatureAvailable(tab)) {
    return false;
  }
  // Home is always shown so the panel never opens to an empty surface.
  if (tab == TabId::Home || m_config == nullptr) {
    return true;
  }
  const auto& hidden = m_config->config().controlCenter.hiddenTabs;
  return !std::ranges::contains(hidden, tabKey(tab));
}

std::vector<ControlCenterPanel::TabCatalogEntry> ControlCenterPanel::hideableTabCatalog() {
  std::vector<TabCatalogEntry> out;
  out.reserve(kTabCount - 1);
  for (const auto& meta : kTabs) {
    if (meta.id == TabId::Home) {
      continue;
    }
    out.push_back({.key = meta.key, .titleKey = meta.titleKey});
  }
  return out;
}

std::string_view ControlCenterPanel::tabKey(TabId tab) {
  for (const auto& meta : kTabs) {
    if (meta.id == tab) {
      return meta.key;
    }
  }
  return {};
}

ControlCenterPanel::TabId ControlCenterPanel::firstVisibleTab() const {
  for (const auto& meta : kTabs) {
    if (isTabVisible(meta.id)) {
      return meta.id;
    }
  }
  return TabId::Home;
}

void ControlCenterPanel::syncTabVisibility() {
  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    const bool visible = isTabVisible(meta.id);
    if (m_tabButtons[idx] != nullptr) {
      m_tabButtons[idx]->setVisible(visible);
    }
    if (!visible) {
      if (m_tabContainers[idx] != nullptr) {
        m_tabContainers[idx]->setVisible(false);
      }
      if (m_tabHeaderActions[idx] != nullptr) {
        m_tabHeaderActions[idx]->setVisible(false);
      }
    }
  }
}

void ControlCenterPanel::updateTabChrome(TabId tab) {
  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    const bool tabEnabled = isTabVisible(meta.id);
    if (m_tabs[idx] != nullptr) {
      m_tabs[idx]->setActive(tabEnabled && meta.id == tab);
    }
    if (m_tabButtons[idx] != nullptr) {
      m_tabButtons[idx]->setVisible(tabEnabled);
      m_tabButtons[idx]->setVariant(meta.id == tab ? ButtonVariant::TabActive : ButtonVariant::Tab);
    }
    if (meta.id == tab && m_contentTitle != nullptr) {
      m_contentTitle->setText(i18n::tr(meta.titleKey));
    }
    if (m_tabHeaderActions[idx] != nullptr) {
      m_tabHeaderActions[idx]->setVisible(tabEnabled && meta.id == tab);
    }
  }

  if (m_contentTitle != nullptr) {
    m_contentTitle->setVisible(true);
  }
  if (m_contentHeaderActions != nullptr) {
    m_contentHeaderActions->setVisible(true);
  }
  if (m_sidebarNav != nullptr) {
    m_sidebarNav->notifyExternalSelectionChanged();
  }
}

void ControlCenterPanel::applyTabContainerVisibility(TabId activeTab) {
  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    const bool tabEnabled = isTabVisible(meta.id);
    if (m_tabContainers[idx] != nullptr) {
      m_tabContainers[idx]->setVisible(tabEnabled && meta.id == activeTab);
    }
  }
}

void ControlCenterPanel::layoutTabContainers(float bodyWidth, float bodyHeight) {
  const float travel = bodyHeight > 0.0f ? bodyHeight : 0.0f;
  for (std::size_t i = 0; i < kTabCount; ++i) {
    auto* container = m_tabContainers[i];
    if (container == nullptr || !container->visible()) {
      continue;
    }

    container->setSize(bodyWidth, bodyHeight);

    float offsetY = 0.0f;
    float opacity = 1.0f;
    const auto tabId = static_cast<TabId>(i);
    if (m_tabTransitionActive && travel > 0.0f) {
      const auto direction = static_cast<float>(m_tabTransitionDirection);
      if (tabId == m_tabTransitionOutgoing) {
        offsetY = -direction * travel * m_tabTransitionProgress;
        opacity = 1.0f - 0.3f * m_tabTransitionProgress;
      } else if (tabId == m_activeTab) {
        offsetY = direction * travel * (1.0f - m_tabTransitionProgress);
        opacity = 0.7f + 0.3f * m_tabTransitionProgress;
      }
    }

    container->setPosition(0.0f, offsetY);
    container->setOpacity(opacity);
    if (m_tabTransitionActive) {
      container->setZIndex(tabId == m_activeTab ? 1 : 0);
    } else {
      container->setZIndex(0);
    }
  }
}

void ControlCenterPanel::resetTabContainerTransforms() {
  for (auto* container : m_tabContainers) {
    if (container == nullptr) {
      continue;
    }
    container->setPosition(0.0f, 0.0f);
    container->setOpacity(1.0f);
    container->setZIndex(0);
  }
}

int ControlCenterPanel::visibleTabOrdinal(TabId tab) const {
  int ordinal = 0;
  for (const auto& meta : kTabs) {
    if (!isTabVisible(meta.id)) {
      continue;
    }
    if (meta.id == tab) {
      return ordinal;
    }
    ++ordinal;
  }
  return 0;
}

void ControlCenterPanel::applyTabTransitionLayout() {
  if (m_tabBodies == nullptr) {
    return;
  }
  layoutTabContainers(m_tabBodies->width(), m_tabBodies->height());
}

void ControlCenterPanel::startTabTransition(TabId from, TabId to) {
  if (m_animations == nullptr || m_tabBodies == nullptr) {
    applyTabContainerVisibility(to);
    resetTabContainerTransforms();
    return;
  }

  m_tabTransitionActive = true;
  m_tabTransitionOutgoing = from;
  m_tabTransitionProgress = 0.0f;

  const int fromOrdinal = visibleTabOrdinal(from);
  const int toOrdinal = visibleTabOrdinal(to);
  m_tabTransitionDirection = toOrdinal >= fromOrdinal ? 1 : -1;

  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    if (m_tabContainers[idx] == nullptr || !isTabVisible(meta.id)) {
      continue;
    }
    const bool show = meta.id == from || meta.id == to;
    m_tabContainers[idx]->setVisible(show);
  }

  applyTabTransitionLayout();
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestRedraw();
  PanelManager::instance().requestFrameTick();

  m_tabTransitionAnimId = m_animations->animate(
      0.0f, 1.0f, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
      [this](float progress) {
        m_tabTransitionProgress = progress;
        applyTabTransitionLayout();
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_tabTransitionAnimId = 0;
        finishTabTransition();
        PanelManager::instance().requestLayout();
        PanelManager::instance().requestRedraw();
      },
      m_tabBodies
  );
}

void ControlCenterPanel::finishTabTransition() {
  m_tabTransitionActive = false;
  resetTabContainerTransforms();
  applyTabContainerVisibility(m_activeTab);
}

void ControlCenterPanel::wireSidebarScroll(InputArea* area) {
  if (area == nullptr) {
    return;
  }
  area->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps == 0.0f) {
      return;
    }
    selectAdjacentVisibleTab(steps > 0.0f ? 1 : -1);
  });
}

void ControlCenterPanel::selectAdjacentVisibleTab(int direction) {
  if (direction == 0) {
    return;
  }

  const int activeOrdinal = visibleTabOrdinal(m_activeTab);
  const int targetOrdinal = activeOrdinal + direction;

  int ordinal = 0;
  for (const auto& meta : kTabs) {
    if (!isTabVisible(meta.id)) {
      continue;
    }
    if (ordinal == targetOrdinal) {
      if (meta.id != m_activeTab) {
        selectTab(meta.id, true);
        PanelManager::instance().refresh();
      }
      return;
    }
    ++ordinal;
  }
}

void ControlCenterPanel::selectTab(TabId tab, bool animated) {
  if (!isTabVisible(tab)) {
    tab = firstVisibleTab();
  }

  const TabId previousTab = m_activeTab;
  const bool tabChanged = tab != previousTab;

  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
    finishTabTransition();
  }

  m_activeTab = tab;
  if (tab == TabId::Notifications && m_notificationManager != nullptr) {
    m_notificationManager->markNotificationHistorySeen();
  }

  updateTabChrome(tab);

  if (tabChanged && animated && m_animations != nullptr && m_tabBodies != nullptr) {
    startTabTransition(previousTab, tab);
  } else {
    m_tabTransitionActive = false;
    applyTabContainerVisibility(tab);
    resetTabContainerTransforms();
  }

  scheduleMprisRefreshFor(tab);
}

void ControlCenterPanel::scheduleMprisRefreshFor(TabId tab) {
  if (m_mpris == nullptr || m_mprisRefreshScheduled || (tab != TabId::Home && tab != TabId::Media)) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_lastMprisRefreshAt.time_since_epoch().count() != 0 && now - m_lastMprisRefreshAt < kMprisRefreshMinInterval) {
    return;
  }

  m_lastMprisRefreshAt = now;
  m_mprisRefreshScheduled = true;
  DeferredCall::callLater([this]() {
    m_mprisRefreshScheduled = false;
    if (m_mpris == nullptr || !PanelManager::instance().isOpenPanel("control-center")) {
      return;
    }
    m_mpris->refreshPlayers();
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

bool ControlCenterPanel::isDirectSectionOpenContext(std::string_view context) const {
  if (context.empty() || context == "home") {
    return false;
  }
  for (const auto& tab : kTabs) {
    if (tab.id != TabId::Home && context == tab.key) {
      return true;
    }
  }
  return false;
}

ControlCenterSidebarMode ControlCenterPanel::sidebarModeForOpen(std::string_view context) const {
  if (m_config == nullptr) {
    return ControlCenterSidebarMode::Compact;
  }
  const auto& cc = m_config->config().controlCenter;
  return isDirectSectionOpenContext(context) ? cc.sidebarSectionMode : cc.sidebarMode;
}

ControlCenterPanel::TabId ControlCenterPanel::tabFromContext(std::string_view context) const {
  for (const auto& tab : kTabs) {
    if (context == tab.key) {
      return isTabVisible(tab.id) ? tab.id : firstVisibleTab();
    }
  }
  return TabId::Home;
}

std::size_t ControlCenterPanel::tabIndex(TabId id) { return static_cast<std::size_t>(id); }

void ControlCenterPanel::layoutFullSidebarWidth(Renderer& renderer) {
  if (m_sidebarScrollView == nullptr || m_sidebarNav == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float fontSize = Style::fontSizeBody * scale;
  const float paddingH = Style::spaceSm * scale * 2.0f;
  const float gap = Style::spaceSm * scale;
  const float glyphW = 21.0f * scale;

  float maxTabWidth = 0.0f;
  for (const auto& meta : kTabs) {
    if (!isTabVisible(meta.id)) {
      continue;
    }
    const TextMetrics text = renderer.measureText(i18n::tr(meta.titleKey), fontSize, FontWeight::Bold);
    maxTabWidth = std::max(maxTabWidth, paddingH + glyphW + gap + text.width);
  }

  const float minWidth = Style::controlHeightSm * scale;
  const float contentWidth = std::max(minWidth, std::ceil(maxTabWidth));

  // Scrollbar gutter lives inside the scroll viewport; reserve it only when the nav overflows.
  float targetWidth = contentWidth;
  const float scrollHeight = m_sidebarScrollView->height();
  if (scrollHeight > 0.0f) {
    LayoutConstraints navConstraints;
    navConstraints.setExactWidth(contentWidth);
    const float navHeight = m_sidebarNav->measure(renderer, navConstraints).height;
    if (navHeight > scrollHeight + 0.5f) {
      targetWidth = contentWidth + Style::scrollbarWidth + Style::scrollbarGap;
    }
  }

  if (std::abs(m_sidebarScrollView->width() - targetWidth) > 0.5f) {
    m_sidebarScrollView->setSize(targetWidth, m_sidebarScrollView->height());
  }
}

void ControlCenterPanel::scrollSidebarNodeIntoView(const Node* node) {
  if (node == nullptr || m_sidebarScrollView == nullptr) {
    return;
  }
  scrollNodeIntoScrollView(*m_sidebarScrollView, &m_sidebarScrollState, *node, Style::spaceXs * contentScale());
  PanelManager::instance().requestLayout();
}

void ControlCenterPanel::scrollFocusedInputIntoView(InputArea* area) {
  if (area == nullptr) {
    return;
  }

  if (m_sidebarScrollView != nullptr && m_sidebarScrollView->content() != nullptr) {
    for (const Node* node = area; node != nullptr; node = node->parent()) {
      if (node == m_sidebarScrollView->content()) {
        scrollNodeIntoScrollView(*m_sidebarScrollView, &m_sidebarScrollState, *area, Style::spaceXs * contentScale());
        PanelManager::instance().requestLayout();
        return;
      }
    }
  }

  if (ScrollView* scrollView = findEnclosingScrollView(area)) {
    scrollNodeIntoScrollView(*scrollView, nullptr, *area, Style::spaceMd * contentScale());
    PanelManager::instance().requestLayout();
  }
}
