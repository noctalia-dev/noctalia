#pragma once

#include "core/file_watcher.h"
#include "core/timer_manager.h"
#include "scripting/plugin_ipc.h"
#include "scripting/script_runtime.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class HttpClient;
class ClipboardService;

namespace scripting {

  class ScriptApiContext;

  // Plugin-level setting overrides keyed by plugin id, then setting key.
  using PluginSettingsMap = std::unordered_map<std::string, ScriptSettings>;

  // Hosts headless [[service]] entries: one singleton runtime per service entry,
  // started at launch and ticked on its own interval. Services hold a plugin's
  // shared/background logic and publish to the per-plugin state store; the
  // plugin's UI entries (widgets, panels) consume it via noctalia.state.
  class PluginServiceHost {
  public:
    PluginServiceHost(
        ScriptApiContext& scriptApi, HttpClient* httpClient, ClipboardService* clipboard, FileWatcher* fileWatcher
    );
    ~PluginServiceHost();

    PluginServiceHost(const PluginServiceHost&) = delete;
    PluginServiceHost& operator=(const PluginServiceHost&) = delete;

    // Discover and launch every [[service]] entry in the registry, seeding each
    // with its plugin-level settings.
    void start(const PluginSettingsMap& pluginSettings);

    // Re-seed running services from the latest plugin settings; restart only those
    // whose effective settings changed. Called on config reload.
    void refresh(const PluginSettingsMap& pluginSettings);

  private:
    // A service is reachable by IPC via the `all` target (it is a singleton with no
    // output): `noctalia msg plugin <author/plugin:entry> all <event> [payload]`.
    struct Service : public PluginIpcEndpoint {
      std::string entryId;
      std::filesystem::path sourcePath;
      std::shared_ptr<ScriptRuntime> runtime;
      ScriptRuntime::SubscriberId subscription = 0;
      FileWatcher::WatchId watchId = 0;
      Timer updateTimer;
      int updateIntervalMs = 1000;
      ScriptSettings lastSeededSettings;
      std::shared_ptr<bool> alive = std::make_shared<bool>(true);

      [[nodiscard]] std::string_view ipcEntryId() const override { return entryId; }
      [[nodiscard]] std::string_view ipcOutputName() const override { return {}; }
      [[nodiscard]] std::string_view ipcBarName() const override { return {}; }
      [[nodiscard]] DispatchResult
      dispatchIpc(std::string_view event, std::string_view payload, const ScriptSnapshot& snapshot) override;
    };

    void armTimer(Service& service);
    void setupScriptWatch(Service& service);
    void teardownScriptWatch(Service& service);
    void reloadService(Service& service);
    // Subscribe to the service's runtime (to track update-interval changes) and arm
    // its update timer. Shared by start() and refresh().
    void subscribeAndArm(Service& service);
    // Build, register, and start a service runtime for an entry. Returns null on an
    // empty/unreadable source.
    [[nodiscard]] std::unique_ptr<Service>
    makeService(const std::string& entryId, const std::filesystem::path& source, ScriptSettings seeded);
    // Full teardown for removal/shutdown: unregister IPC, stop timer + runtime, and
    // mark the alive token dead so any in-flight callback is a no-op.
    void stopService(Service& service);
    // Build the effective seeded settings for an entry (manifest defaults + plugin
    // overrides). Returns nullopt if the entry no longer resolves.
    [[nodiscard]] std::optional<ScriptSettings>
    seedFor(const std::string& entryId, const PluginSettingsMap& pluginSettings) const;

    ScriptApiContext& m_scriptApi;
    HttpClient* m_httpClient = nullptr;
    ClipboardService* m_clipboard = nullptr;
    FileWatcher* m_fileWatcher = nullptr;
    std::vector<std::unique_ptr<Service>> m_services;
  };

} // namespace scripting
