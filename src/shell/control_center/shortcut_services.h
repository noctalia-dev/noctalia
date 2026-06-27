#pragma once

class BluetoothService;
class CompositorPlatform;
class ConfigService;
class DependencyService;
class FileWatcher;
class IdleInhibitor;
class INetworkService;
class MprisService;
class GammaService;
class IpcService;
class NotificationManager;
class PipeWireService;
class PowerProfilesService;
class WeatherService;
class HttpClient;
class ClipboardService;

namespace noctalia::theme {
  class ThemeService;
}
namespace scripting {
  class ScriptApiContext;
}

struct ShortcutServices {
  INetworkService* network = nullptr;
  BluetoothService* bluetooth = nullptr;
  GammaService* nightLight = nullptr;
  noctalia::theme::ThemeService* theme = nullptr;
  NotificationManager* notifications = nullptr;
  IdleInhibitor* idleInhibitor = nullptr;
  PipeWireService* audio = nullptr;
  PowerProfilesService* powerProfiles = nullptr;
  MprisService* mpris = nullptr;
  WeatherService* weather = nullptr;
  ConfigService* config = nullptr;
  DependencyService* dependencies = nullptr;
  CompositorPlatform* platform = nullptr;
  IpcService* ipc = nullptr;
  // Plugin shortcut runtime dependencies.
  scripting::ScriptApiContext* scriptApi = nullptr;
  FileWatcher* fileWatcher = nullptr;
  HttpClient* httpClient = nullptr;
  ClipboardService* clipboard = nullptr;
};
