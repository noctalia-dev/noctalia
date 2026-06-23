#include "shell/control_center/shortcut_registry.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_glyphs.h"
#include "dbus/power/power_profiles_service.h"
#include "i18n/i18n.h"
#include "idle/idle_inhibitor.h"
#include "ipc/ipc_service.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_service.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"
#include "shell/bar/widgets/keyboard_layout_widget.h"
#include "shell/control_center/plugin_shortcut.h"
#include "shell/control_center/shortcut_services.h"
#include "shell/panel/panel_manager.h"
#include "system/gamma_service.h"
#include "system/weather_service.h"
#include "theme/theme_service.h"

#include <array>
#include <cmath>
#include <deque>
#include <format>
#include <optional>
#include <vector>

namespace {

  constexpr std::array<ShortcutRegistry::CatalogEntry, 17> kShortcutCatalog{{
      {"wifi", "control-center.shortcuts.wifi"},
      {"bluetooth", "control-center.shortcuts.bluetooth"},
      {"nightlight", "control-center.shortcuts.nightlight"},
      {"notification", "control-center.shortcuts.notification"},
      {"dark_mode", "control-center.shortcuts.dark-mode.dark"},
      {"caffeine", "control-center.shortcuts.caffeine"},
      {"audio", "control-center.shortcuts.audio"},
      {"mic_mute", "control-center.shortcuts.mic-mute"},
      {"power_profile", "control-center.shortcuts.power-profile"},
      {"media", "control-center.shortcuts.media"},
      {"weather", "control-center.shortcuts.weather"},
      {"system", "control-center.shortcuts.system"},
      {"screen_time", "control-center.shortcuts.screen-time"},
      {"keyboard_layout", "control-center.shortcuts.keyboard-layout"},
      {"wallpaper", "control-center.shortcuts.wallpaper"},
      {"session", "control-center.shortcuts.session"},
      {"clipboard", "control-center.shortcuts.clipboard"},
  }};

  void openTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  // ── Toggle shortcuts ────────────────────────────────────────────────────────

  class WifiShortcut final : public Shortcut {
  public:
    explicit WifiShortcut(INetworkService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "wifi"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.wifi"); }
    std::string displayLabel() const override {
      if (m_svc != nullptr) {
        const NetworkState& state = m_svc->state();
        if (state.kind == NetworkConnectivity::Wireless && state.connected && !state.ssid.empty()) {
          return state.ssid;
        }
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override { return "wifi"; }
    std::string_view iconOff() const override { return "wifi-off"; }
    std::string displayIcon() const override {
      if (m_svc == nullptr) {
        return "wifi-question";
      }
      return network_glyphs::wifiGlyphForState(m_svc->state());
    }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->state().wirelessEnabled; }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->setWirelessEnabled(!m_svc->state().wirelessEnabled);
      }
    }
    void onRightClick() override { openTab("network"); }

  private:
    INetworkService* m_svc;
  };

  class BluetoothShortcut final : public Shortcut {
  public:
    explicit BluetoothShortcut(BluetoothService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "bluetooth"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.bluetooth"); }
    std::string_view iconOn() const override { return "bluetooth"; }
    std::string_view iconOff() const override { return "bluetooth-off"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->state().powered; }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->setPowered(!m_svc->state().powered);
      }
    }
    void onRightClick() override { openTab("bluetooth"); }

  private:
    BluetoothService* m_svc;
  };

  class NightlightShortcut final : public Shortcut {
  public:
    NightlightShortcut(GammaService* svc, CompositorPlatform* platform) : m_svc(svc), m_platform(platform) {}
    std::string_view id() const override { return "nightlight"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.nightlight"); }
    bool enabled() const override { return m_platform != nullptr && m_platform->hasGammaControl(); }
    std::string displayLabel() const override {
      if (m_svc == nullptr) {
        return defaultLabel();
      }
      if (m_svc->forceEnabled()) {
        return i18n::tr("control-center.shortcuts.nightlight-states.forced");
      }
      if (!m_svc->enabled()) {
        return i18n::tr("control-center.shortcuts.nightlight-states.off");
      }
      // Scheduled and currently warming the screen.
      if (m_svc->active()) {
        return i18n::tr("control-center.shortcuts.nightlight-states.scheduled-night");
      }
      // Scheduled but in the day phase: surface that the click "took" even
      // though the service is in day phase.
      return i18n::tr("control-center.shortcuts.nightlight-states.scheduled-day");
    }
    std::string_view iconOn() const override {
      return m_svc != nullptr && m_svc->forceEnabled() ? "nightlight-forced" : "nightlight-on";
    }
    std::string_view iconOff() const override { return "nightlight-off"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && (m_svc->forceEnabled() || m_svc->active()); }
    void onClick() override {
      if (!enabled() || m_svc == nullptr) {
        return;
      }
      // Mirror the bar widget: primary toggles on/off; if currently forced,
      // drop force and land on scheduled-on so force is reversible without
      // also losing the master enable.
      if (m_svc->forceEnabled()) {
        m_svc->clearForceOverride();
        m_svc->setEnabled(true);
      } else {
        m_svc->toggleEnabled();
      }
    }
    void onRightClick() override {
      if (enabled() && m_svc != nullptr) {
        m_svc->toggleForceEnabled();
      }
    }

  private:
    GammaService* m_svc;
    CompositorPlatform* m_platform;
  };

  class NotificationShortcut final : public Shortcut {
  public:
    explicit NotificationShortcut(NotificationManager* svc) : m_svc(svc) {}
    std::string_view id() const override { return "notification"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.notification"); }
    std::string_view iconOn() const override { return "bell-off"; }
    std::string_view iconOff() const override { return "bell"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->doNotDisturb(); }
    void onClick() override {
      if (m_svc != nullptr) {
        (void)m_svc->toggleDoNotDisturb();
      }
    }
    void onRightClick() override { openTab("notifications"); }

  private:
    NotificationManager* m_svc;
  };

  class DarkModeShortcut final : public Shortcut {
  public:
    explicit DarkModeShortcut(noctalia::theme::ThemeService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "dark_mode"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.dark-mode.dark"); }
    std::string displayLabel() const override {
      if (m_svc == nullptr) {
        return defaultLabel();
      }
      switch (m_svc->configuredMode()) {
      case ThemeMode::Dark:
        return i18n::tr("control-center.shortcuts.dark-mode.dark");
      case ThemeMode::Light:
        return i18n::tr("control-center.shortcuts.dark-mode.light");
      case ThemeMode::Auto:
        return i18n::tr("control-center.shortcuts.dark-mode.auto");
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override {
      return m_svc != nullptr && m_svc->configuredMode() == ThemeMode::Auto ? "theme-mode" : "weather-moon-stars";
    }
    std::string_view iconOff() const override { return "weather-sun"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->configuredMode() != ThemeMode::Light; }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->cycleMode();
      }
    }

  private:
    noctalia::theme::ThemeService* m_svc;
  };

  class IdleInhibitorShortcut final : public Shortcut {
  public:
    explicit IdleInhibitorShortcut(IdleInhibitor* svc) : m_svc(svc) {}
    std::string_view id() const override { return "caffeine"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.caffeine"); }
    std::string_view iconOn() const override { return "caffeine-on"; }
    std::string_view iconOff() const override { return "caffeine-off"; }
    bool isToggle() const override { return true; }
    bool active() const override { return m_svc != nullptr && m_svc->enabled(); }
    void onClick() override {
      if (m_svc != nullptr) {
        m_svc->toggle();
      }
    }

  private:
    IdleInhibitor* m_svc;
  };

  class AudioShortcut final : public Shortcut {
  public:
    explicit AudioShortcut(PipeWireService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "audio"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.audio"); }
    std::string_view iconOn() const override { return "volume-x"; }
    std::string_view iconOff() const override { return "volume-high"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const AudioNode* sink = m_svc->defaultSink();
      return sink != nullptr && sink->muted;
    }
    void onClick() override {
      if (m_svc != nullptr) {
        if (const AudioNode* sink = m_svc->defaultSink(); sink != nullptr) {
          m_svc->setMuted(!sink->muted);
        }
      }
    }
    void onRightClick() override { openTab("audio"); }

  private:
    PipeWireService* m_svc;
  };

  class MicMuteShortcut final : public Shortcut {
  public:
    explicit MicMuteShortcut(PipeWireService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "mic_mute"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.mic-mute"); }
    std::string_view iconOn() const override { return "microphone-mute"; }
    std::string_view iconOff() const override { return "microphone"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const AudioNode* source = m_svc->defaultSource();
      return source != nullptr && source->muted;
    }
    void onClick() override {
      if (m_svc != nullptr) {
        if (const AudioNode* source = m_svc->defaultSource(); source != nullptr) {
          m_svc->setMicMuted(!source->muted);
        }
      }
    }
    void onRightClick() override { openTab("audio"); }

  private:
    PipeWireService* m_svc;
  };

  const WidgetConfig* findKeyboardLayoutWidgetConfig(const Config& config) {
    auto resolve = [&config](const std::string& name) -> const WidgetConfig* {
      const auto it = config.widgets.find(name);
      if (it == config.widgets.end() || it->second.type != "keyboard_layout") {
        return nullptr;
      }
      return &it->second;
    };

    auto search = [&resolve](const std::vector<std::string>& widgets) -> const WidgetConfig* {
      for (const std::string& name : widgets) {
        if (const WidgetConfig* wc = resolve(name); wc != nullptr) {
          return wc;
        }
      }
      return nullptr;
    };

    for (const BarConfig& bar : config.bars) {
      if (const WidgetConfig* wc = search(bar.startWidgets); wc != nullptr) {
        return wc;
      }
      if (const WidgetConfig* wc = search(bar.centerWidgets); wc != nullptr) {
        return wc;
      }
      if (const WidgetConfig* wc = search(bar.endWidgets); wc != nullptr) {
        return wc;
      }
    }

    return resolve("keyboard_layout");
  }

  KeyboardLayoutWidget::DisplayMode keyboardLayoutDisplayMode(const ConfigService* config) {
    if (config == nullptr) {
      return KeyboardLayoutWidget::DisplayMode::Short;
    }
    const WidgetConfig* wc = findKeyboardLayoutWidgetConfig(config->config());
    const std::string display = wc != nullptr ? wc->getString("display", "short") : std::string("short");
    return KeyboardLayoutWidget::parseDisplayMode(display);
  }

  class PowerProfileShortcut final : public Shortcut {
  public:
    explicit PowerProfileShortcut(PowerProfilesService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "power_profile"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.power-profile"); }
    std::string displayLabel() const override {
      if (m_svc != nullptr && !m_svc->activeProfile().empty()) {
        return profileLabel(m_svc->activeProfile());
      }
      return defaultLabel();
    }
    std::string_view iconOn() const override {
      return profileGlyphName(m_svc != nullptr ? m_svc->activeProfile() : "");
    }
    std::string_view iconOff() const override { return "balanced"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      return m_svc != nullptr && !m_svc->activeProfile().empty() && m_svc->activeProfile() != "balanced";
    }
    void onClick() override { cycle(1); }
    void onRightClick() override { cycle(-1); }
    void onScroll(int direction) override { cycle(direction); }

  private:
    void cycle(int direction) {
      if (m_svc != nullptr) {
        (void)m_svc->cycleActiveProfile(direction);
      }
    }

    PowerProfilesService* m_svc;
  };

  class WeatherShortcut final : public Shortcut {
  public:
    explicit WeatherShortcut(WeatherService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "weather"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.weather"); }
    std::string displayLabel() const override {
      if (m_svc != nullptr && m_svc->enabled() && m_svc->hasData()) {
        const auto& snapshot = m_svc->snapshot();
        const int temp = static_cast<int>(std::lround(m_svc->displayTemperature(snapshot.current.temperatureC)));
        return std::format("{}{}", temp, m_svc->displayTemperatureUnit());
      }
      return defaultLabel();
    }
    std::string displayIcon() const override {
      if (m_svc == nullptr || !m_svc->enabled()) {
        return "weather-cloud-off";
      }
      if (m_svc->hasData()) {
        const auto& snapshot = m_svc->snapshot();
        return WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
      }
      return "weather-cloud";
    }
    std::string_view iconOn() const override { return "weather-cloud-sun"; }
    std::string_view iconOff() const override { return "weather-cloud-sun"; }
    void onClick() override { openTab("weather"); }
    void onRightClick() override { openTab("weather"); }

  private:
    WeatherService* m_svc;
  };

  class KeyboardLayoutShortcut final : public Shortcut {
  public:
    KeyboardLayoutShortcut(CompositorPlatform* platform, ConfigService* config)
        : m_platform(platform), m_config(config) {}
    std::string_view id() const override { return "keyboard_layout"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.keyboard-layout"); }
    std::string displayLabel() const override {
      return KeyboardLayoutWidget::formatLayoutLabel(resolvedLayoutName(), keyboardLayoutDisplayMode(m_config));
    }
    std::string_view iconOn() const override { return "keyboard"; }
    std::string_view iconOff() const override { return "keyboard"; }
    void onClick() override {
      if (m_platform != nullptr) {
        (void)m_platform->cycleKeyboardLayout();
      }
      PanelManager::instance().refresh();
    }

  private:
    [[nodiscard]] std::string resolvedLayoutName() const {
      const auto state = m_platform != nullptr ? m_platform->keyboardLayoutState() : std::nullopt;
      if (state.has_value()
          && state->currentIndex >= 0
          && state->currentIndex < static_cast<int>(state->names.size())) {
        return state->names[static_cast<std::size_t>(state->currentIndex)];
      }

      if (m_platform != nullptr) {
        return m_platform->currentKeyboardLayoutName();
      }

      return {};
    }

    CompositorPlatform* m_platform = nullptr;
    ConfigService* m_config = nullptr;
  };

  // ── Action-only shortcuts ────��──────────────────────────────────────────────

  class MediaShortcut final : public Shortcut {
  public:
    explicit MediaShortcut(MprisService* svc) : m_svc(svc) {}
    std::string_view id() const override { return "media"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.media"); }
    std::string_view iconOn() const override { return "media-pause"; }
    std::string_view iconOff() const override { return "media-play"; }
    bool isToggle() const override { return true; }
    bool active() const override {
      if (m_svc == nullptr) {
        return false;
      }
      const auto active = m_svc->activePlayer();
      return active.has_value() && active->playbackStatus == "Playing";
    }
    void onClick() override {
      if (m_svc != nullptr) {
        (void)m_svc->playPauseActive();
      }
    }
    void onRightClick() override { openTab("media"); }

  private:
    MprisService* m_svc;
  };

  class SystemShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "system"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.system"); }
    std::string_view iconOn() const override { return "activity"; }
    std::string_view iconOff() const override { return "activity"; }
    void onClick() override { openTab("system"); }
    void onRightClick() override { openTab("system"); }
  };

  class ScreenTimeShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "screen_time"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.screen-time"); }
    std::string_view iconOn() const override { return "hourglass"; }
    std::string_view iconOff() const override { return "hourglass"; }
    void onClick() override { openTab("screen-time"); }
    void onRightClick() override { openTab("screen-time"); }
  };

  class WallpaperShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "wallpaper"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.wallpaper"); }
    std::string_view iconOn() const override { return "wallpaper-selector"; }
    std::string_view iconOff() const override { return "wallpaper-selector"; }
    void onClick() override { PanelManager::instance().togglePanel("wallpaper"); }
  };

  class SessionShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "session"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.session"); }
    std::string_view iconOn() const override { return "shutdown"; }
    std::string_view iconOff() const override { return "shutdown"; }
    void onClick() override { PanelManager::instance().togglePanel("session"); }
  };

  class ClipboardShortcut final : public Shortcut {
  public:
    std::string_view id() const override { return "clipboard"; }
    std::string defaultLabel() const override { return i18n::tr("control-center.shortcuts.clipboard"); }
    std::string_view iconOn() const override { return "clipboard"; }
    std::string_view iconOff() const override { return "clipboard"; }
    void onClick() override { PanelManager::instance().togglePanel("clipboard"); }
  };

} // namespace

std::span<const ShortcutRegistry::CatalogEntry> ShortcutRegistry::catalog() {
  // Built-in shortcuts plus every plugin [[shortcut]] entry. Plugin id/label
  // strings are held in a stable static deque so the CatalogEntry views stay valid.
  static std::deque<std::string> storage;
  static const std::vector<CatalogEntry> combined = [] {
    std::vector<CatalogEntry> result(kShortcutCatalog.begin(), kShortcutCatalog.end());
    scripting::PluginRegistry::instance().ensureScanned();
    for (const auto& entry :
         scripting::PluginRegistry::instance().entriesOfKind(scripting::PluginEntryKind::Shortcut)) {
      storage.push_back(entry.fullId());
      const std::string_view typeView = storage.back();
      storage.push_back(entry.manifest->name.empty() ? entry.fullId() : entry.manifest->name);
      const std::string_view labelView = storage.back();
      result.push_back(CatalogEntry{.type = typeView, .labelKey = labelView, .literalLabel = true});
    }
    return result;
  }();
  return combined;
}

std::unique_ptr<Shortcut> ShortcutRegistry::create(std::string_view type, const ShortcutServices& s) {
  if (auto entry = scripting::PluginRegistry::instance().resolve(type);
      entry.has_value() && entry->entry->kind == scripting::PluginEntryKind::Shortcut) {
    if (s.scriptApi == nullptr) {
      return nullptr;
    }
    auto seeded = scripting::seedEntrySettings(*entry->entry, {});
    static const std::unordered_map<std::string, WidgetSettingValue> kNoPluginOverrides;
    const auto* overrides = &kNoPluginOverrides;
    if (s.config != nullptr) {
      const auto& pluginSettings = s.config->config().plugins.pluginSettings;
      if (const auto psIt = pluginSettings.find(entry->manifest->id); psIt != pluginSettings.end()) {
        overrides = &psIt->second;
      }
    }
    scripting::mergePluginSettings(*entry->manifest, *overrides, seeded);
    return std::make_unique<PluginShortcut>(scripting::PluginRuntimeContext{
        .entryId = entry->fullId(),
        .sourcePath = entry->sourcePath,
        .settings = std::move(seeded),
        .scriptApi = *s.scriptApi,
        .fileWatcher = s.fileWatcher,
        .httpClient = s.httpClient,
        .clipboard = s.clipboard,
        .platform = s.platform,
    });
  }
  if (type == "wifi")
    return std::make_unique<WifiShortcut>(s.network);
  if (type == "bluetooth")
    return std::make_unique<BluetoothShortcut>(s.bluetooth);
  if (type == "nightlight")
    return std::make_unique<NightlightShortcut>(s.nightLight, s.platform);
  if (type == "notification")
    return std::make_unique<NotificationShortcut>(s.notifications);
  if (type == "dark_mode")
    return std::make_unique<DarkModeShortcut>(s.theme);
  if (type == "caffeine")
    return std::make_unique<IdleInhibitorShortcut>(s.idleInhibitor);
  if (type == "audio")
    return std::make_unique<AudioShortcut>(s.audio);
  if (type == "mic_mute")
    return std::make_unique<MicMuteShortcut>(s.audio);
  if (type == "power_profile")
    return std::make_unique<PowerProfileShortcut>(s.powerProfiles);
  if (type == "media")
    return std::make_unique<MediaShortcut>(s.mpris);
  if (type == "weather") {
    if (s.config != nullptr && !s.config->config().weather.enabled) {
      return nullptr;
    }
    return std::make_unique<WeatherShortcut>(s.weather);
  }
  if (type == "system") {
    if (s.config != nullptr && !s.config->config().system.monitor.enabled) {
      return nullptr;
    }
    return std::make_unique<SystemShortcut>();
  }
  if (type == "screen_time") {
    if (s.config != nullptr && !s.config->config().shell.screenTimeEnabled) {
      return nullptr;
    }
    return std::make_unique<ScreenTimeShortcut>();
  }
  if (type == "keyboard_layout")
    return std::make_unique<KeyboardLayoutShortcut>(s.platform, s.config);
  if (type == "wallpaper")
    return std::make_unique<WallpaperShortcut>();
  if (type == "session")
    return std::make_unique<SessionShortcut>();
  if (type == "clipboard") {
    if (s.config != nullptr && !s.config->config().shell.clipboardEnabled) {
      return nullptr;
    }
    return std::make_unique<ClipboardShortcut>();
  }
  return nullptr;
}
