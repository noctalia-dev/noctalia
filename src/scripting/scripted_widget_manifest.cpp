#include "scripting/scripted_widget_manifest.h"

#include "core/resource_paths.h"
#include "scripting/luau_host.h"
#include "scripting/scripted_widget_bindings.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace scripting {

  namespace {

    std::string readFile(const std::filesystem::path& path) {
      std::ifstream f(path);
      if (!f) {
        return {};
      }
      std::stringstream ss;
      ss << f.rdbuf();
      return ss.str();
    }

    struct CacheEntry {
      std::filesystem::file_time_type mtime{};
      std::uintmax_t size = 0;
      std::optional<ScriptWidgetManifest> manifest;
    };

    std::optional<ScriptWidgetManifest> extractUncached(const std::filesystem::path& resolvedScript) {
      const std::string source = readFile(resolvedScript);
      if (source.empty()) {
        return std::nullopt;
      }

      LuauHost host;
      ScriptedWidgetBindingContext context;
      ScriptWidgetManifest manifest;
      context.host = &host;
      context.manifestExtractionMode = true;
      context.manifestOut = &manifest;
      host.setScriptContext(&context);
      registerScriptedWidgetBindings(host.state(), &context);

      // `barWidget.define` aborts the chunk via luaL_error, so run() returning
      // false is the expected, successful path. Success is signalled by
      // context.defineCalled.
      host.setMuteErrors(true);
      context.beginCall({});
      (void)host.exec(resolvedScript.filename().string(), source);
      host.setMuteErrors(false);

      if (!context.defineCalled) {
        return std::nullopt;
      }
      return manifest;
    }

  } // namespace

  std::filesystem::path resolveScriptPath(const std::string& path) {
    if (path.empty()) {
      return {};
    }
    if (path[0] == '~') {
      if (const char* home = std::getenv("HOME"); home != nullptr) {
        return std::string(home) + path.substr(1);
      }
      return path;
    }
    if (path[0] == '/') {
      return path;
    }
    return paths::assetPath(path);
  }

  std::optional<ScriptWidgetManifest> extractScriptManifest(const std::filesystem::path& resolvedScript) {
    if (resolvedScript.empty()) {
      return std::nullopt;
    }

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(resolvedScript, ec);
    if (ec) {
      return std::nullopt;
    }
    const auto size = std::filesystem::file_size(resolvedScript, ec);
    if (ec) {
      return std::nullopt;
    }

    static std::mutex cacheMutex;
    static std::unordered_map<std::string, CacheEntry> cache;
    const std::string key = resolvedScript.string();

    {
      std::lock_guard lock(cacheMutex);
      if (const auto it = cache.find(key); it != cache.end() && it->second.mtime == mtime && it->second.size == size) {
        return it->second.manifest;
      }
    }

    std::optional<ScriptWidgetManifest> manifest = extractUncached(resolvedScript);

    {
      std::lock_guard lock(cacheMutex);
      cache[key] = CacheEntry{.mtime = mtime, .size = size, .manifest = manifest};
    }
    return manifest;
  }

  std::optional<ScriptWidgetManifest> manifestForScriptConfig(const std::string& scriptConfigValue) {
    if (scriptConfigValue.empty()) {
      return std::nullopt;
    }
    return extractScriptManifest(resolveScriptPath(scriptConfigValue));
  }

  std::vector<DiscoveredScript> discoverBundledScriptedWidgets() {
    std::vector<DiscoveredScript> discovered;

    const std::filesystem::path scriptsDir = paths::assetPath("scripts");
    std::error_code ec;
    if (!std::filesystem::is_directory(scriptsDir, ec)) {
      return discovered;
    }

    for (const auto& entry : std::filesystem::directory_iterator(scriptsDir, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
        continue;
      }
      auto manifest = extractScriptManifest(entry.path());
      if (!manifest.has_value() || !manifest->pickable) {
        continue;
      }
      discovered.push_back(
          DiscoveredScript{
              .id = entry.path().stem().string(),
              .assetScript = "scripts/" + entry.path().filename().string(),
              .manifest = std::move(*manifest),
          }
      );
    }
    return discovered;
  }

} // namespace scripting
