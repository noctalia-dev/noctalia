#pragma once

class ClipboardService;
class ConfigService;
class FileWatcher;
class HttpClient;
class MprisService;
class PipeWireService;
class PipeWireSpectrum;
class RenderContext;
class SharedTextureCache;
class SystemMonitorService;
class WaylandConnection;
class WeatherService;

namespace scripting {
  class ScriptApiContext;
}

// Dependencies for plugin-backed (`[[desktop_widget]]`) widgets. All-null means
// plugin desktop widgets are unavailable in this factory (e.g. tests).
struct DesktopWidgetScriptDeps {
  scripting::ScriptApiContext* scriptApi = nullptr;
  FileWatcher* fileWatcher = nullptr;
  ClipboardService* clipboard = nullptr;
  ConfigService* configService = nullptr;
};

struct DesktopWidgetRuntimeServices {
  PipeWireService* pipewire = nullptr;
  PipeWireSpectrum* pipewireSpectrum = nullptr;
  const WeatherService* weather = nullptr;
  MprisService* mpris = nullptr;
  HttpClient* httpClient = nullptr;
  SystemMonitorService* sysmon = nullptr;
  DesktopWidgetScriptDeps scriptDeps;
};

struct DesktopWidgetServices {
  WaylandConnection& wayland;
  ConfigService* config = nullptr;
  RenderContext* renderContext = nullptr;
  DesktopWidgetRuntimeServices runtime;
  SharedTextureCache* textureCache = nullptr;
};
