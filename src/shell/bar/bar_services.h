#pragma once

class IslandActivities;
class BluetoothService;
class BrightnessService;
class ClipboardService;
class CompositorPlatform;
class ConfigService;
class EasyEffectsService;
class ExternalIpService;
class FileWatcher;
class HttpClient;
class IdleInhibitor;
class INetworkService;
class LockKeysService;
class ModemManagerService;
class MprisService;
class NotificationManager;
class JobProgressManager;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class RenderContext;
class ScreenshotService;
class SystemMonitorService;
class TrayService;
class UPowerService;
class WeatherService;
class Wallpaper;
class GammaService;

namespace nythera::theme {
  class ThemeService;
}
namespace scripting {
  class ScriptApiContext;
}

struct BarServices {
  CompositorPlatform& platform;
  ConfigService& config;
  NotificationManager* notifications = nullptr;
  JobProgressManager* jobs = nullptr;
  IslandActivities* activities = nullptr;
  TrayService* tray = nullptr;
  PipeWireService* audio = nullptr;
  EasyEffectsService* easyEffects = nullptr;
  UPowerService* upower = nullptr;
  SystemMonitorService* sysmon = nullptr;
  PowerProfilesService* powerProfiles = nullptr;
  INetworkService* network = nullptr;
  ModemManagerService* modem = nullptr;
  ExternalIpService* externalIp = nullptr;
  IdleInhibitor* idleInhibitor = nullptr;
  MprisService* mpris = nullptr;
  PipeWireSpectrum* audioSpectrum = nullptr;
  HttpClient* httpClient = nullptr;
  WeatherService* weather = nullptr;
  RenderContext* renderContext = nullptr;
  GammaService* nightLight = nullptr;
  nythera::theme::ThemeService* theme = nullptr;
  BluetoothService* bluetooth = nullptr;
  BrightnessService* brightness = nullptr;
  LockKeysService* lockKeys = nullptr;
  ClipboardService* clipboard = nullptr;
  FileWatcher* fileWatcher = nullptr;
  ScreenshotService* screenshots = nullptr;
  Wallpaper* wallpaper = nullptr;
  scripting::ScriptApiContext* scriptApi = nullptr;
};
