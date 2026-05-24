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
}

ControlCenterPanel::ControlCenterPanel(
    NotificationManager* notifications, PipeWireService* audio, MprisService* mpris, ConfigService* config,
    HttpClient* httpClient, WeatherService* weather, PipeWireSpectrum* spectrum, UPowerService* upower,
    PowerProfilesService* powerProfiles, INetworkService* network, NetworkSecretAgent* networkSecrets,
    BluetoothService* bluetooth, BluetoothAgent* bluetoothAgent, BrightnessService* brightness,
    SystemMonitorService* sysmon, ScreenTimeService* screenTime, GammaService* nightLight,
    noctalia::theme::ThemeService* theme, IdleInhibitor* idleInhibitor, DependencyService* dependencies,
    CompositorPlatform* platform, Wallpaper* wallpaper
) {
  (void)upower;
  WaylandConnection* wayland = platform != nullptr ? &platform->wayland() : nullptr;
  m_config = config;
  m_mpris = mpris;
  m_notificationManager = notifications;
  m_dependencies = dependencies;
  m_tabs[tabIndex(TabId::Home)] = std::make_unique<HomeTab>(
      mpris, httpClient, weather, audio, powerProfiles, config, network, bluetooth, nightLight, theme, notifications,
      idleInhibitor, dependencies, platform, wallpaper
  );
  m_tabs[tabIndex(TabId::Media)] = std::make_unique<MediaTab>(
      mpris, httpClient, spectrum, config, wayland, PanelManager::instance().renderContext()
  );
  m_tabs[tabIndex(TabId::Audio)] =
      std::make_unique<AudioTab>(audio, mpris, config, wayland, PanelManager::instance().renderContext());
  m_tabs[tabIndex(TabId::Weather)] = std::make_unique<WeatherTab>(weather, config);
  m_tabs[tabIndex(TabId::Calendar)] = std::make_unique<CalendarTab>(config);
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
  const bool compact = m_config != nullptr && m_config->config().controlCenter.compact;
  return scaled(compact ? 660.0f : 780.0f);
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
  m_compact = m_config != nullptr && m_config->config().controlCenter.compact;

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
                  selectTab(id);
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
          .configure = [scale, opacity = panelCardOpacity()](
                           Button& button
                       ) { panel_button_style::configureHeaderIconButton(button, scale, opacity); },
      })
  );
  header->addChild(std::move(headerActions));

  content->addChild(std::move(header));

  auto bodies = ui::column({
      .out = &m_tabBodies,
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
      .flexGrow = 1.0f,
  });

  for (std::size_t i = 0; i < kTabCount; ++i) {
    auto container = m_tabs[i]->create();
    container->setFlexGrow(1.0f);
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
  if (m_closeButton != nullptr) {
    panel_button_style::applyHeaderButtonStyle(*m_closeButton, opacity);
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

  const std::size_t activeIdx = tabIndex(m_activeTab);
  if (m_tabs[activeIdx] != nullptr) {
    m_tabs[activeIdx]->layout(renderer, bodyWidth, bodyHeight);
  }
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
  selectTab(tabFromContext(context));
}

bool ControlCenterPanel::isContextActive(std::string_view context) const {
  return m_activeTab == tabFromContext(context);
}

void ControlCenterPanel::onClose() {
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

void ControlCenterPanel::selectTab(TabId tab) {
  if (!isTabVisible(tab)) {
    tab = firstVisibleTab();
  }
  m_activeTab = tab;
  if (tab == TabId::Notifications && m_notificationManager != nullptr) {
    m_notificationManager->markNotificationHistorySeen();
  }
  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    const bool tabEnabled = isTabVisible(meta.id);
    if (m_tabContainers[idx] != nullptr) {
      m_tabContainers[idx]->setVisible(tabEnabled && meta.id == tab);
    }
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

ControlCenterPanel::TabId ControlCenterPanel::tabFromContext(std::string_view context) const {
  for (const auto& tab : kTabs) {
    if (context == tab.key) {
      return isTabVisible(tab.id) ? tab.id : firstVisibleTab();
    }
  }
  return TabId::Home;
}

std::size_t ControlCenterPanel::tabIndex(TabId id) { return static_cast<std::size_t>(id); }
