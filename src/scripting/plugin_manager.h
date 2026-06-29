#pragma once

#include "config/config_types.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class ConfigService;

namespace scripting {

  class PluginRegistry;

  // Resolve the [plugins] config into registry source roots + an enabled gate and
  // (re)scan. Pure disk work — never exports git files (no network). Shared by
  // PluginManager::refresh and the config-validate CLI so both resolve plugin
  // widget types against the same active set.
  void applyPluginSourcesToRegistry(PluginRegistry& registry, const PluginsConfig& plugins);

  struct EnableResult {
    bool ok = false;
    std::string error;
  };

  struct PluginStatus {
    std::string id;
    std::string name;
    std::string version;
    std::string icon;
    std::string description;
    std::string license = "MIT";
    std::vector<std::string> dependencies;
    std::string source; // source name ("local" for the implicit dev source)
    bool compatible = true;
    bool deprecated = false;
    bool enabled = false;
    bool materialized = false;
  };

  // Owns the plugin distribution lifecycle: resolves the configured sources into
  // registry source roots + an enabled gate, and drives enable/disable. The
  // implicit local dev source (the user data dir) is always active; managed git /
  // path sources are gated by [plugins].enabled. Construct one as an Application
  // member and subscribe refresh() as an early config-reload callback so the
  // registry updates before bar / control-center rebuilds.
  class PluginManager {
  public:
    explicit PluginManager(ConfigService& config) : m_config(config) {}

    // Called after an out-of-band registry change that isn't a config reload — i.e. a
    // git `update()` that advanced a source. Lets Application rebuild the bar and
    // reconcile services for the new revision. Enable/disable already propagate via the
    // config-reload path, so they don't use this.
    void setOnChanged(std::function<void()> onChanged) { m_onChanged = std::move(onChanged); }

    // Called when a plugin starts or finishes its background git export (the in-flight
    // set queried by isEnabling() changed). Lets the settings UI redraw the row spinner.
    void setOnEnablingChanged(std::function<void()> cb) { m_onEnablingChanged = std::move(cb); }

    // Resolve source roots + enabled filter from config and (re)scan the registry.
    // No-op when the plugins config is unchanged since the last applied refresh.
    void refresh();

    // Enable a plugin by id ("author/plugin"). For a git source the runtime export
    // lazy-fetches blobs from the blobless clone (seconds, network-bound), so it runs
    // on a worker thread: enable() returns immediately, isEnabling(id) is true until
    // the export lands, then it persists + refreshes on the main thread. Path / local
    // dev plugins are validated and persisted inline (no network). The synchronous
    // result reports only the inline-validation outcome; a git export reports ok and
    // surfaces later failures via the log.
    [[nodiscard]] EnableResult enable(std::string_view pluginId);

    // Whether a git-source plugin's background export is currently in flight.
    [[nodiscard]] bool isEnabling(std::string_view pluginId) const;

    // Disable a plugin by id and persist. Code stays on disk; settings are retained.
    void disable(std::string_view pluginId);

    // Disable and remove a plugin's materialized files from disk.
    void remove(std::string_view pluginId);

    // Every plugin offered by the local dev source + each configured source, with
    // its compatibility and active state. For the management CLI / settings browser.
    [[nodiscard]] std::vector<PluginStatus> list() const;
    [[nodiscard]] std::vector<PluginStatus> list(const PluginsConfig& plugins) const;

    // Add (or replace) a source and refresh.
    void addSource(const PluginSourceConfig& source);

    // Fetch a git source off-thread, export compatible enabled plugins, keep
    // incompatible enabled plugins on their previous exported copy, then advance
    // the source catalog. If the fetched revision is already current, reconcile
    // any held exports that are now compatible. Re-scans on the main thread. No-op
    // for path / unknown sources.
    void update(std::string sourceName);

    // Remove a source: delete its git repo cache and exported runtime files, disable
    // its plugins, drop it from config. Path sources keep their externally-owned
    // directory.
    void removeSource(std::string sourceName);

  private:
    [[nodiscard]] std::optional<PluginSourceConfig> findSource(std::string_view name) const;
    // Plugin ids offered by the implicit local dev source.
    [[nodiscard]] std::unordered_set<std::string> localPluginIds() const;
    // Re-derive any enabled git-source plugin missing from disk. Present repos are
    // materialized synchronously (local git, no network); a wiped repo is re-cloned and
    // materialized on a worker thread so startup never blocks on the network. Returns
    // whether anything was exported synchronously. No network when nothing is missing.
    bool ensureEnabledMaterialized(const PluginsConfig& plugins) const;
    // Export the enabled plugins a present repo ships, from local git data only.
    bool materializeEnabledFromRepo(
        const PluginSourceConfig& source, const std::filesystem::path& repoRoot, const std::vector<std::string>& enabled
    ) const;
    // Clone a missing source repo and materialize its enabled plugins off the main
    // thread, rebuilding the bar via m_onChanged once the export lands.
    void spawnCloneAndMaterialize(
        PluginSourceConfig source, std::filesystem::path repoRoot, std::vector<std::string> enabled
    ) const;

    ConfigService& m_config;
    std::function<void()> m_onChanged;
    std::function<void()> m_onEnablingChanged;
    // Git-source plugins whose runtime export is running on a worker thread. Touched
    // only on the main thread (enable() inserts, the DeferredCall completion erases).
    std::unordered_set<std::string> m_enabling;
    PluginsConfig m_lastApplied;
    bool m_applied = false;
  };

} // namespace scripting
