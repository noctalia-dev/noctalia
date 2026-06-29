#pragma once

#include "config/config_types.h"

#include <filesystem>
#include <string>
#include <unordered_map>

class ClipboardService;
class CompositorPlatform;
class FileWatcher;
class HttpClient;
class MprisService;
class PipeWireSpectrum;

namespace scripting {

  class ScriptApiContext;

  struct PluginRuntimeContext {
    std::string entryId;
    std::filesystem::path sourcePath = {};
    std::unordered_map<std::string, WidgetSettingValue> settings;
    ScriptApiContext& scriptApi;
    FileWatcher* fileWatcher = nullptr;
    HttpClient* httpClient = nullptr;
    ClipboardService* clipboard = nullptr;
    CompositorPlatform* platform = nullptr;
    PipeWireSpectrum* audioSpectrum = nullptr;
    MprisService* mpris = nullptr;
  };

} // namespace scripting
