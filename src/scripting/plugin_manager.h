#pragma once

#include "config/config_types.h"
#include "scripting/plugin_catalog.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
    std::string version;          // installed version when materialized, else the resolved version
    std::string availableVersion; // resolved version offered when updateAvailable (else empty)
    std::string icon;
    std::string description;
    std::string license = "MIT";
    std::vector<std::string> dependencies;
    std::string source; // source name ("local" for the implicit dev source)
    bool compatible = true;
    bool deprecated = false;
    bool enabled = false;
    bool materialized = false;
    // An enabled+materialized git-source plugin whose fetched catalog version differs
    // from the exported copy on disk — a newer release is available to apply via update().
    bool updateAvailable = false;
    // The source's newest version targets an unsupported plugin API, so an older release
    // is installed instead. `latestVersion` / `latestPluginApiVersion` describe that
    // newest version, so the UI can name what a Noctalia upgrade would unlock.
    bool heldBack = false;
    std::string latestVersion;
    std::uint32_t latestPluginApiVersion = 0;
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

    // Called when update() advances a git source to a new revision. Lets the plugin store
    // drop its cached thumbnail/README copies for that source so they re-fetch at the new
    // HEAD instead of showing the previous revision's files.
    void setOnSourceUpdated(std::function<void(const std::string& sourceName)> cb) {
      m_onSourceUpdated = std::move(cb);
    }

    // Called after a disabled plugin has been enabled successfully.
    void setOnEnabled(std::function<void(std::string_view pluginId)> cb) { m_onEnabled = std::move(cb); }

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
    // `access` gates git catalog reads: Network (clone / lazy-fetch, worker threads
    // only) or LocalOnly (safe on the main thread, e.g. the IPC handler).
    [[nodiscard]] std::vector<PluginStatus> list(CatalogAccess access) const;
    [[nodiscard]] std::vector<PluginStatus> list(const PluginsConfig& plugins, CatalogAccess access) const;

    // Throttled `git fetch` of the enabled git sources in `plugins`, so the settings
    // browser / store show newly published plugins on open without waiting for the
    // 6h auto-update. A source fetched within the throttle window is skipped. Blocking
    // git/IO — call off the UI thread with a main-thread config snapshot. Only advances
    // FETCH_HEAD (browsable catalog); it never advances HEAD or exports files.
    void fetchStaleCatalogs(const PluginsConfig& plugins);

    // Update enabled git sources scoped by the background auto-update mode
    // (None = nothing, Official = only the built-in official source, All = every
    // enabled git source). Backs the background auto-update tick and the store's
    // "update all" action (All).
    void updateAutoUpdateScope(PluginAutoUpdateMode mode);

    // Set the global background auto-update mode ([plugins].auto_update).
    // Backs the "auto-update plugins" settings dropdown; the mode is applied
    // per source by the auto-update tick.
    void setAutoUpdateMode(PluginAutoUpdateMode mode);

    // Add (or replace) a source and refresh.
    void addSource(const PluginSourceConfig& source);

    // Fetch a git source off-thread and export compatible enabled plugins that its
    // catalog owns by exact id. Incompatible or failed plugin exports keep their
    // previous copy without blocking the source revision or unrelated exports; later
    // updates reconcile copies that do not match the catalog. Re-scans on the main
    // thread. No-op for path / unknown sources.
    void update(std::string sourceName);

    // Remove a source: delete its git repo cache and exported runtime files, disable
    // its plugins, drop it from config. Path sources keep their externally-owned
    // directory.
    void removeSource(std::string sourceName);

  private:
    [[nodiscard]] std::optional<PluginSourceConfig> findSource(std::string_view name) const;
    // The work behind update(name), on the exact configured source. Callers that
    // already hold the matched PluginSourceConfig must use this directly rather
    // than resolving by name again.
    void updateSource(const PluginSourceConfig& source);
    // Plugin ids offered by the implicit local dev source.
    [[nodiscard]] std::unordered_set<std::string> localPluginIds() const;
    // Re-derive any enabled git-source plugin missing from disk, per source on a worker
    // thread (catalog reads and exports lazy-fetch blobs from the blobless clone, so
    // even a present repo can hit the network); startup never blocks on it.
    void ensureEnabledMaterialized(const PluginsConfig& plugins) const;
    // Export the enabled plugins a present repo ships. Reads and exports can lazy-fetch
    // blobs (network-bound); worker threads only.
    bool materializeEnabledFromRepo(
        const PluginSourceConfig& source, const std::filesystem::path& repoRoot, const std::vector<std::string>& enabled
    ) const;
    // Worker thread: optionally re-clone the source repo, then materialize its enabled
    // plugins, rebuilding the bar via m_onChanged once an export lands.
    void spawnMaterializeEnabled(
        PluginSourceConfig source, std::filesystem::path repoRoot, std::vector<std::string> enabled, bool cloneFirst
    ) const;

    ConfigService& m_config;
    std::function<void()> m_onChanged;
    std::function<void()> m_onEnablingChanged;
    std::function<void(const std::string& sourceName)> m_onSourceUpdated;
    std::function<void(std::string_view pluginId)> m_onEnabled;
    // Git-source plugins whose runtime export is running on a worker thread. Touched
    // only on the main thread (enable() inserts, the DeferredCall completion erases).
    std::unordered_set<std::string> m_enabling;
    PluginsConfig m_lastApplied;
    bool m_applied = false;

    // Wall-clock of the last browse fetch per git source name; throttles store-open
    // fetches. Touched from worker threads (settings refresh / store open), so guarded.
    std::mutex m_browseFetchMutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastBrowseFetch;
  };

} // namespace scripting
