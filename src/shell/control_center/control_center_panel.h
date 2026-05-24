#pragma once

#include "shell/control_center/audio_tab.h"
#include "shell/control_center/bluetooth_tab.h"
#include "shell/control_center/calendar_tab.h"
#include "shell/control_center/display_tab.h"
#include "shell/control_center/home_tab.h"
#include "shell/control_center/media_tab.h"
#include "shell/control_center/network_tab.h"
#include "shell/control_center/notifications_tab.h"
#include "shell/control_center/system_tab.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/weather_tab.h"
#include "shell/panel/panel.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

class BluetoothAgent;
class BluetoothService;
class BrightnessService;
class Button;
class CompositorPlatform;
class ConfigService;
class DependencyService;
class Flex;
class HttpClient;
class IdleInhibitor;
class InputArea;
class Label;
class MprisService;
class NetworkSecretAgent;
class INetworkService;
class GammaService;
class NotificationManager;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class ScreenTimeService;
class SystemMonitorService;
class UPowerService;
class Wallpaper;
class WeatherService;

namespace noctalia::theme {
  class ThemeService;
}

class ControlCenterPanel : public Panel {
public:
  ControlCenterPanel(
      NotificationManager* notifications, PipeWireService* audio, MprisService* mpris, ConfigService* config = nullptr,
      HttpClient* httpClient = nullptr, WeatherService* weather = nullptr, PipeWireSpectrum* spectrum = nullptr,
      UPowerService* upower = nullptr, PowerProfilesService* powerProfiles = nullptr,
      INetworkService* network = nullptr, NetworkSecretAgent* networkSecrets = nullptr,
      BluetoothService* bluetooth = nullptr, BluetoothAgent* bluetoothAgent = nullptr,
      BrightnessService* brightness = nullptr, SystemMonitorService* sysmon = nullptr,
      ScreenTimeService* screenTime = nullptr, GammaService* nightLight = nullptr,
      noctalia::theme::ThemeService* theme = nullptr, IdleInhibitor* idleInhibitor = nullptr,
      DependencyService* dependencies = nullptr, CompositorPlatform* platform = nullptr, Wallpaper* wallpaper = nullptr
  );

  void create() override;
  void onFrameTick(float deltaMs) override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  [[nodiscard]] bool dismissTransientUi();
  [[nodiscard]] bool isContextActive(std::string_view context) const override;
  [[nodiscard]] bool deferExternalRefresh() const override;
  [[nodiscard]] bool deferPointerRelayout() const override;

  [[nodiscard]] float preferredWidth() const override;

  [[nodiscard]] float preferredHeight() const override { return scaled(520.0f); }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;

private:
  void onPanelBordersChanged(bool enabled) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  enum class TabId : std::uint8_t {
    Home,
    Media,
    Audio,
    Display,
    System,
    Network,
    Bluetooth,
    Weather,
    Calendar,
    Notifications,
    ScreenTime,
    Count,
  };

  struct TabMeta {
    TabId id;
    const char* key;
    const char* titleKey;
    const char* glyph;
  };

  static constexpr std::size_t kTabCount = static_cast<std::size_t>(TabId::Count);
  static constexpr std::array<TabMeta, kTabCount> kTabs{{
      {TabId::Home, "home", "control-center.tabs.home", "home"},
      {TabId::Media, "media", "control-center.tabs.media", "disc-filled"},
      {TabId::Audio, "audio", "control-center.tabs.audio", "volume"},
      {TabId::Display, "display", "control-center.tabs.display", "device-desktop"},
      {TabId::System, "system", "control-center.tabs.system", "activity-heartbeat"},
      {TabId::Network, "network", "control-center.tabs.network", "wifi"},
      {TabId::Bluetooth, "bluetooth", "control-center.tabs.bluetooth", "bluetooth"},
      {TabId::Weather, "weather", "control-center.tabs.weather", "weather-cloud-sun"},
      {TabId::Calendar, "calendar", "control-center.tabs.calendar", "calendar"},
      {TabId::Notifications, "notifications", "control-center.tabs.notifications", "bell"},
      {TabId::ScreenTime, "screen-time", "control-center.tabs.screen-time", "hourglass"},
  }};

  void selectTab(TabId tab);
  void scheduleMprisRefreshFor(TabId tab);
  void syncTabVisibility();
  [[nodiscard]] bool isTabVisible(TabId tab) const;
  [[nodiscard]] TabId firstVisibleTab() const;
  [[nodiscard]] TabId tabFromContext(std::string_view context) const;
  [[nodiscard]] static std::size_t tabIndex(TabId id);

  // Tab instances (long-lived, survive panel open/close cycles)
  std::array<std::unique_ptr<Tab>, kTabCount> m_tabs;

  // Panel UI structure (rebuilt each create(), nulled in onClose())
  Flex* m_rootLayout = nullptr;
  Flex* m_sidebar = nullptr;
  Flex* m_content = nullptr;
  InputArea* m_contentDismissArea = nullptr;
  Flex* m_contentHeader = nullptr;
  Flex* m_contentHeaderActions = nullptr;
  Label* m_contentTitle = nullptr;
  Button* m_closeButton = nullptr;
  Flex* m_tabBodies = nullptr;
  std::array<Button*, kTabCount> m_tabButtons{};
  std::array<Flex*, kTabCount> m_tabContainers{};
  std::array<Flex*, kTabCount> m_tabHeaderActions{};
  TabId m_activeTab = TabId::Home;
  ConfigService* m_config = nullptr;
  MprisService* m_mpris = nullptr;
  NotificationManager* m_notificationManager = nullptr;
  DependencyService* m_dependencies = nullptr;
  bool m_compact = false;
  bool m_mprisRefreshScheduled = false;
  std::chrono::steady_clock::time_point m_lastMprisRefreshAt{};
};
