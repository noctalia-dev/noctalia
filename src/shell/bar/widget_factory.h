#pragma once

#include "shell/bar/widget.h"

#include <memory>
#include <string>

struct Config;
class FileWatcher;
class CompositorPlatform;
class NotificationManager;
class HttpClient;
class IdleInhibitor;
class LockKeysService;
class MprisService;
class BluetoothService;
class BrightnessService;
class ClipboardService;
class INetworkService;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class TrayService;
class SystemMonitorService;
class UPowerService;
class WeatherService;
struct wl_output;
class GammaService;
namespace noctalia::theme {
  class ThemeService;
}

class WidgetFactory {
public:
  WidgetFactory(
      CompositorPlatform& platform, const Config& config, NotificationManager* notifications, TrayService* tray,
      PipeWireService* audio, UPowerService* upower, SystemMonitorService* sysmon, PowerProfilesService* powerProfiles,
      INetworkService* network, IdleInhibitor* idleInhibitor, MprisService* mpris, PipeWireSpectrum* audioSpectrum,
      HttpClient* httpClient, WeatherService* weather, GammaService* nightLight,
      noctalia::theme::ThemeService* themeService, BluetoothService* bluetooth, BrightnessService* brightness,
      LockKeysService* lockKeys, ClipboardService* clipboard, FileWatcher* fileWatcher = nullptr
  );
  ~WidgetFactory();

  [[nodiscard]] std::unique_ptr<Widget> create(
      const std::string& name, wl_output* output, float contentScale = 1.0f, const std::string& barPosition = "top",
      const std::string& barName = "default", float widgetSpacing = 6.0f
  ) const;

private:
  CompositorPlatform& m_platform;
  const Config& m_config;
  NotificationManager* m_notifications;
  TrayService* m_tray;
  PipeWireService* m_audio;
  UPowerService* m_upower;
  SystemMonitorService* m_sysmon;
  PowerProfilesService* m_powerProfiles;
  INetworkService* m_network;
  IdleInhibitor* m_idleInhibitor;
  MprisService* m_mpris;
  PipeWireSpectrum* m_audioSpectrum;
  HttpClient* m_httpClient;
  WeatherService* m_weather;
  GammaService* m_nightLight;
  noctalia::theme::ThemeService* m_themeService;
  BluetoothService* m_bluetooth;
  BrightnessService* m_brightness;
  LockKeysService* m_lockKeys;
  ClipboardService* m_clipboard;
  FileWatcher* m_fileWatcher;
};
