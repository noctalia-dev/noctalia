#include "shell/control_center/control_center_panel.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/control_center/screen_time_tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/dependency_service.h"
#include "system/screen_time_service.h"
#include "ui/builders.h"

#include <chrono>
#include <memory>
#include <optional>

using namespace control_center;

namespace {
  constexpr auto kMprisRefreshMinInterval = std::chrono::milliseconds(750);

  [[nodiscard]] float preferredWidthForSidebarMode(ControlCenterSidebarMode mode, float scale) {
    switch (mode) {
    case ControlCenterSidebarMode::Full:
      return 780.0f * scale;
    case ControlCenterSidebarMode::Compact:
      return 660.0f * scale;
    case ControlCenterSidebarMode::None:
      return 600.0f * scale;
    }
    return 660.0f * scale;
  }
} // namespace

ControlCenterPanel::ControlCenterPanel(
    NotificationManager* notifications, PipeWireService* audio, MprisService* mpris, ConfigService* config,
    HttpClient* httpClient, WeatherService* weather, PipeWireSpectrum* spectrum, UPowerService* upower,
    PowerProfilesService* powerProfiles, INetworkService* network, NetworkSecretAgent* networkSecrets,
    BluetoothService* bluetooth, BluetoothAgent* bluetoothAgent, BrightnessService* brightness,
    SystemMonitorService* sysmon, ScreenTimeService* screenTime, GammaService* nightLight,
    noctalia::theme::ThemeService* theme, IdleInhibitor* idleInhibitor, DependencyService* dependencies,
    CompositorPlatform* platform, IpcService* ipc, Wallpaper* wallpaper, CalendarService* calendar
) {
  (void)upower;
  WaylandConnection* wayland = platform != nullptr ? &platform->wayland() : nullptr;
  m_config = config;
  m_mpris = mpris;
  m_notificationManager = notifications;
  m_dependencies = dependencies;
  m_tabs[tabIndex(TabId::Home)] = std::make_unique<HomeTab>(
      mpris, httpClient, weather, audio, powerProfiles, config, network, bluetooth, nightLight, theme, notifications,
      idleInhibitor, dependencies, platform, ipc, wallpaper
  );
  m_tabs[tabIndex(TabId::Media)] = std::make_unique<MediaTab>(
      mpris, httpClient, spectrum, config, wayland, PanelManager::instance().renderContext()
  );
  m_tabs[tabIndex(TabId::Audio)] =
      std::make_unique<AudioTab>(audio, mpris, config, wayland, PanelManager::instance().renderContext());
  m_tabs[tabIndex(TabId::Weather)] = std::make_unique<WeatherTab>(weather, config);
  m_tabs[tabIndex(TabId::Calendar)] = std::make_unique<CalendarTab>(config, calendar);
  m_tabs[tabIndex(TabId::Notifications)] = std::make_unique<NotificationsTab>(notifications);
  m_tabs[tabIndex(TabId::Network)] = std::make_unique<NetworkTab>(network, networkSecrets);
  m_tabs[tabIndex(TabId::Bluetooth)] = std::make_unique<BluetoothTab>(bluetooth, bluetoothAgent);
  m_tabs[tabIndex(TabId::Display)] = std::make_unique<DisplayTab>(brightness, config);
  m_tabs[tabIndex(TabId::System)] = std::make_unique<SystemTab>(sysmon);
  m_tabs[tabIndex(TabId::ScreenTime)] = std::make_unique<ScreenTimeTab>(screenTime);
  m_tabButtons.fill(nullptr);
  m_tabContainers.fill(nullptr);
  m_tabHeaderActions.fill(nullptr);
}

float ControlCenterPanel::preferredWidth() const {
  return preferredWidthForSidebarMode(sidebarModeForOpen(pendingOpenContext()), m_contentScale);
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
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceSm * scale,
        .fillHeight = true,
        .configure = [this, scale](Flex& column) {
          column.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()));
          column.setRadius(Style::scaledRadiusXl(scale));
        },
    });

    for (const auto& tab : kTabs) {
      sidebar->addChild(
          ui::button({
              .out = &m_tabButtons[tabIndex(tab.id)],
              .text = m_compact ? std::optional<std::string>{} : std::optional<std::string>{i18n::tr(tab.titleKey)},
              .glyph = tab.glyph,
              .glyphSize = 21.0f * scale,
              .contentAlign = m_compact ? ButtonContentAlign::Center : ButtonContentAlign::Start,
              .variant = ButtonVariant::Tab,
              .minWidth = m_compact ? std::optional<float>{Style::controlHeight * scale} : std::optional<float>{},
              .minHeight = Style::controlHeight * scale,
              .paddingV = Style::spaceSm * scale,
              .paddingH = (m_compact ? Style::spaceSm : Style::spaceMd) * scale,
              .gap = Style::spaceSm * scale,
              .radius = Style::scaledRadiusLg(scale),
              .onClick =
                  [this, id = tab.id]() {
                    selectTab(id, true);
                    PanelManager::instance().refresh();
                  },
              .configure =
                  [scale](Button& button) {
                    if (button.label() != nullptr) {
                      button.label()->setFontWeight(FontWeight::Bold);
                      button.label()->setFontSize(Style::fontSizeBody * scale);
                    }
                  },
          })
      );
    }
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
          .color = colorSpecFromRole(ColorRole::Primary),
          .fontWeight = FontWeight::Bold,
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

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  const float contentInnerWidth =
      std::max(0.0f, m_content->width() - (m_content->paddingLeft() + m_content->paddingRight()));
  const float bodyWidth = m_tabBodies->width();
  const float bodyHeight = m_tabBodies->height();

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

bool ControlCenterPanel::isTabVisible(TabId tab) const {
  if (m_config == nullptr) {
    switch (tab) {
    case TabId::ScreenTime:
      return false;
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
  default:
    return true;
  }
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
    const TabId tabId = static_cast<TabId>(i);
    if (m_tabTransitionActive && travel > 0.0f) {
      const float direction = static_cast<float>(m_tabTransitionDirection);
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
