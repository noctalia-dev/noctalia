#include "scripting/plugin_manager.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "scripting/plugin_api.h"
#include "scripting/plugin_catalog.h"
#include "scripting/plugin_git.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_source_locks.h"
#include "scripting/plugin_source_paths.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <ranges>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scripting {

  namespace {
    constexpr Logger kLog("plugins");

    std::filesystem::path sourceRootFor(const PluginSourceConfig& source) { return plugin_paths::registryRoot(source); }

    void removeManagedGitSourceStorage(const PluginSourceConfig& source) {
      const std::string sourceBase = FileUtils::pluginSourcesDir();
      if (!sourceBase.empty()) {
        (void)plugin_paths::removeTreeUnder(plugin_paths::sourceStorageRoot(source), sourceBase);
      }
      const std::string materializedBase = FileUtils::pluginMaterializedDir();
      if (!materializedBase.empty()) {
        (void)plugin_paths::removeTreeUnder(plugin_paths::gitMaterializedRoot(source), materializedBase);
      }
    }

    bool sourceReplacementInvalidatesGitStorage(const PluginSourceConfig& previous, const PluginSourceConfig& next) {
      if (previous.kind == PluginSourceKind::Git && next.kind == PluginSourceKind::Git) {
        return previous.location != next.location;
      }
      return previous.kind == PluginSourceKind::Git || next.kind == PluginSourceKind::Git;
    }

    std::filesystem::path materializedPluginDir(const PluginSourceConfig& source, std::string_view pluginId) {
      const auto subdir = pluginSubdirFromId(pluginId);
      if (!subdir.has_value()) {
        return {};
      }
      const auto root = plugin_paths::gitMaterializedRoot(source);
      return root.empty() ? std::filesystem::path{} : root / *subdir;
    }

    std::filesystem::path uniqueTempRoot(const std::filesystem::path& root, std::string_view subdir) {
      for (int i = 0; i < 8; ++i) {
        std::string suffix = StringUtils::generateUuid();
        if (suffix.empty()) {
          suffix = std::to_string(i);
        }
        std::filesystem::path candidate = root / (".tmp-" + std::string(subdir) + "-" + suffix);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
          return candidate;
        }
      }
      return {};
    }

    bool replaceDirectory(
        const std::filesystem::path& stagedDir, const std::filesystem::path& finalDir,
        const std::filesystem::path& materializedRoot, std::string* error
    ) {
      if (!plugin_paths::pathIsInside(stagedDir, materializedRoot)
          || !plugin_paths::pathIsInside(finalDir, materializedRoot)) {
        if (error != nullptr) {
          *error = "refusing to replace plugin outside materialized source root";
        }
        return false;
      }

      std::error_code ec;
      const auto backupDir =
          materializedRoot / (".old-" + finalDir.filename().string() + "-" + StringUtils::generateUuid());
      bool backedUp = false;
      if (std::filesystem::exists(finalDir, ec)) {
        std::filesystem::rename(finalDir, backupDir, ec);
        if (ec) {
          if (error != nullptr) {
            *error = "failed to move previous plugin copy: " + ec.message();
          }
          return false;
        }
        backedUp = true;
      }

      ec.clear();
      std::filesystem::rename(stagedDir, finalDir, ec);
      if (ec) {
        if (backedUp) {
          std::error_code restoreEc;
          std::filesystem::rename(backupDir, finalDir, restoreEc);
        }
        if (error != nullptr) {
          *error = "failed to install plugin copy: " + ec.message();
        }
        return false;
      }

      if (backedUp) {
        (void)plugin_paths::removeTreeUnder(backupDir, materializedRoot);
      }
      return true;
    }

    struct MaterializeResult {
      bool ok = false;
      std::string error;
      int exitCode = -1;
      bool timedOut = false;
      bool incompatible = false;
      std::uint32_t pluginApiVersion = 0;
      std::filesystem::path pluginDir;
      PluginManifest manifest;

      explicit operator bool() const { return ok; }
    };

    MaterializeResult materializeFailure(
        std::string error, int exitCode = -1, bool timedOut = false, bool incompatible = false,
        std::uint32_t pluginApiVersion = 0
    ) {
      MaterializeResult result;
      result.error = std::move(error);
      result.exitCode = exitCode;
      result.timedOut = timedOut;
      result.incompatible = incompatible;
      result.pluginApiVersion = pluginApiVersion;
      return result;
    }

    MaterializeResult materializeGitPlugin(
        const PluginSourceConfig& source, const std::filesystem::path& repoRoot, std::string_view rev,
        std::string_view pluginId, bool requireCompatible = false
    ) {
      const auto subdir = pluginSubdirFromId(pluginId);
      if (!subdir.has_value()) {
        return materializeFailure("invalid plugin id");
      }
      const auto materializedRoot = plugin_paths::gitMaterializedRoot(source);
      if (materializedRoot.empty()) {
        return materializeFailure("empty materialized source root");
      }

      std::error_code ec;
      std::filesystem::create_directories(materializedRoot, ec);
      if (ec) {
        return materializeFailure("failed to create materialized source root: " + ec.message());
      }

      const auto tmpRoot = uniqueTempRoot(materializedRoot, *subdir);
      if (tmpRoot.empty()) {
        return materializeFailure("failed to allocate temporary plugin export directory");
      }

      const auto cleanupTmp = [&] { (void)plugin_paths::removeTreeUnder(tmpRoot, materializedRoot); };
      const auto exported = plugin_git::exportSubdir(repoRoot, rev, *subdir, tmpRoot);
      if (!exported) {
        cleanupTmp();
        return materializeFailure("export failed: " + exported.err, exported.exitCode, exported.timedOut);
      }

      const auto stagedDir = tmpRoot / *subdir;
      std::string manifestError;
      auto manifest = parsePluginManifest(stagedDir / "plugin.toml", &manifestError);
      if (!manifest.has_value()) {
        cleanupTmp();
        return materializeFailure(manifestError);
      }
      if (manifest->id != pluginId) {
        cleanupTmp();
        return materializeFailure("manifest id '" + manifest->id + "' does not match requested id");
      }
      if (requireCompatible && !supportsPluginApiVersion(manifest->pluginApiVersion)) {
        std::string error = "plugin '"
            + manifest->id
            + "' targets plugin API "
            + std::to_string(manifest->pluginApiVersion)
            + " (supported range "
            + std::to_string(kOldestSupportedPluginApiVersion)
            + "-"
            + std::to_string(kCurrentPluginApiVersion)
            + ")";
        const std::uint32_t pluginApiVersion = manifest->pluginApiVersion;
        cleanupTmp();
        return materializeFailure(std::move(error), -1, false, true, pluginApiVersion);
      }

      const auto finalDir = materializedRoot / *subdir;
      std::string replaceError;
      if (!replaceDirectory(stagedDir, finalDir, materializedRoot, &replaceError)) {
        cleanupTmp();
        return materializeFailure(replaceError);
      }
      cleanupTmp();
      MaterializeResult result;
      result.ok = true;
      result.pluginDir = finalDir;
      result.manifest = std::move(*manifest);
      return result;
    }

    struct ReadGitCatalogResult {
      bool ok = false;
      std::string error;
      std::vector<CatalogEntry> entries;
    };

    ReadGitCatalogResult readGitCatalog(const std::filesystem::path& repoRoot, std::string_view rev = "HEAD") {
      const auto shown = plugin_git::showFile(repoRoot, "catalog.toml", rev);
      if (!shown) {
        return {.ok = false, .error = shown.err, .entries = {}};
      }
      return {.ok = true, .error = {}, .entries = parseCatalogToml(shown.out, rev)};
    }

    // Whether the exported copy on disk is already the revision this host resolves to.
    // Compares the resolved row, not the catalog tip: a held-back plugin sits on an older
    // version on purpose and must not read as a permanently pending update.
    bool materializedPluginMatchesCatalog(
        const PluginSourceConfig& source, std::string_view pluginId, const CatalogEntry& entry
    ) {
      std::string error;
      const auto manifest = parsePluginManifest(materializedPluginDir(source, pluginId) / "plugin.toml", &error);
      if (!manifest.has_value() || manifest->id != pluginId) {
        return false;
      }
      return manifest->version == entry.resolvedVersion && manifest->pluginApiVersion == entry.resolvedPluginApiVersion;
    }

    // Announce which revision an export will land when it is not the catalog tip, so the
    // choice is visible in the log and not only in the settings UI.
    void logHeldBack(const PluginSourceConfig& source, const CatalogEntry& entry) {
      if (!entry.heldBack) {
        return;
      }
      kLog.info(
          "plugin source '{}': installing '{}' v{} (plugin API {}); v{} needs plugin API {} (supported range {}-{})",
          source.name, entry.id, entry.resolvedVersion, entry.resolvedPluginApiVersion, entry.version,
          entry.pluginApiVersion, kOldestSupportedPluginApiVersion, kCurrentPluginApiVersion
      );
    }

    // Version string of the exported (installed) copy on disk, or empty if absent.
    std::string materializedPluginVersion(const PluginSourceConfig& source, std::string_view pluginId) {
      std::string error;
      const auto manifest = parsePluginManifest(materializedPluginDir(source, pluginId) / "plugin.toml", &error);
      return manifest.has_value() ? manifest->version : std::string{};
    }
  } // namespace

  void applyPluginSourcesToRegistry(PluginRegistry& registry, const PluginsConfig& plugins) {
    // Scan every configured source + the local dev dir; a plugin is active only if
    // its id is in [plugins].enabled (opt-in, uniform across all sources).
    //
    // Root order is lowest-to-highest precedence: built-in defaults, then user-added
    // sources (later config entries override earlier), then the local data dir last.
    // The registry keeps the last copy of a duplicate id, so a cloned official /
    // community repo added as a later source overrides the built-in one without
    // touching plugin ids, and a drop-in under the data dir overrides everything.
    std::vector<std::filesystem::path> roots;
    std::unordered_set<std::string> enabled;
    for (const auto& source : plugins.sources) {
      if (!source.enabled) {
        continue;
      }
      if (auto root = sourceRootFor(source); !root.empty()) {
        roots.push_back(std::move(root));
      }
    }
    if (auto localRoot = plugin_paths::localSourceRoot(); !localRoot.empty()) {
      roots.push_back(std::move(localRoot));
    }
    for (const auto& id : plugins.enabled) {
      if (isValidPluginId(id)) {
        enabled.insert(id);
      }
    }
    registry.setSources(std::move(roots));
    registry.setEnabledFilter(std::move(enabled));
    registry.scan();
  }

  std::optional<PluginSourceConfig> PluginManager::findSource(std::string_view name) const {
    for (const auto& source : m_config.config().plugins.sources) {
      if (source.name == name) {
        return source;
      }
    }
    return std::nullopt;
  }

  std::unordered_set<std::string> PluginManager::localPluginIds() const {
    std::unordered_set<std::string> ids;
    const std::string data = FileUtils::dataDir();
    if (data.empty()) {
      return ids;
    }
    PluginSourceConfig localSource{
        .kind = PluginSourceKind::Path, .name = "local", .location = (std::filesystem::path(data) / "plugins").string()
    };
    for (const auto& entry : discoverCatalog(localSource, CatalogAccess::LocalOnly).entries) {
      ids.insert(entry.id);
    }
    return ids;
  }

  void PluginManager::ensureEnabledMaterialized(const PluginsConfig& plugins) const {
    std::error_code ec;
    for (const auto& source : plugins.sources) {
      if (source.kind != PluginSourceKind::Git || !source.enabled) {
        continue;
      }
      const std::filesystem::path repoRoot = plugin_paths::gitRepoRoot(source);
      if (repoRoot.empty()) {
        continue;
      }
      // Even with the repo present, catalog reads and exports lazy-fetch blobs from the
      // blobless clone (network-bound), so reconciliation always runs off the main
      // thread; the registry rescan + bar rebuild marshal back when an export lands.
      const bool cloneFirst = !std::filesystem::exists(repoRoot / ".git", ec);
      if (cloneFirst) {
        // Source repo is gone (state dir wiped) or its first clone never completed.
        std::filesystem::create_directories(repoRoot.parent_path(), ec);
      }
      spawnMaterializeEnabled(source, repoRoot, plugins.enabled, cloneFirst);
    }
  }

  bool PluginManager::materializeEnabledFromRepo(
      const PluginSourceConfig& source, const std::filesystem::path& repoRoot, const std::vector<std::string>& enabled
  ) const {
    bool materialized = false;
    std::error_code ec;
    const auto catalogResult = readGitCatalog(repoRoot);
    if (!catalogResult.ok) {
      kLog.warn("plugin source '{}': cannot read applied catalog: {}", source.name, catalogResult.error);
      return false;
    }
    const auto& catalog = catalogResult.entries;
    for (const auto& id : enabled) {
      const auto sub = pluginSubdirFromId(id);
      if (!sub.has_value()) {
        kLog.warn("skipping enabled plugin with invalid id '{}'", id);
        continue;
      }
      const auto* catalogEntry = findCatalogEntry(catalog, id);
      if (catalogEntry == nullptr) {
        continue; // this exact canonical id is not owned by this source
      }
      const bool hasMaterialized = std::filesystem::exists(materializedPluginDir(source, id) / "plugin.toml", ec);
      if (!catalogEntry->compatible) {
        if (!hasMaterialized) {
          kLog.warn(
              "plugin source '{}': cannot export enabled plugin '{}'; it targets plugin API {} (supported range {}-{})",
              source.name, id, catalogEntry->pluginApiVersion, kOldestSupportedPluginApiVersion,
              kCurrentPluginApiVersion
          );
        }
        continue;
      }
      if (hasMaterialized && materializedPluginMatchesCatalog(source, id, *catalogEntry)) {
        continue; // already materialized at the revision this host resolves to
      }
      if (!plugin_git::hasPath(repoRoot, *sub + "/plugin.toml", catalogEntry->resolvedRevision)) {
        kLog.warn("plugin source '{}': catalog entry '{}' has no {}/plugin.toml", source.name, id, *sub);
        continue;
      }
      kLog.info("exporting enabled plugin '{}' from source '{}'", id, source.name);
      logHeldBack(source, *catalogEntry);
      const auto materializedPlugin = materializeGitPlugin(source, repoRoot, catalogEntry->resolvedRevision, id, true);
      if (materializedPlugin) {
        materialized = true;
      } else if (materializedPlugin.incompatible) {
        kLog.warn(
            "plugin source '{}': cannot export enabled plugin '{}'; it targets plugin API {} (supported range {}-{})",
            source.name, id, materializedPlugin.pluginApiVersion, kOldestSupportedPluginApiVersion,
            kCurrentPluginApiVersion
        );
      } else if (materializedPlugin.timedOut) {
        kLog.warn("plugin source '{}': exporting '{}' timed out", source.name, id);
      } else {
        kLog.warn(
            "plugin source '{}': exporting '{}' failed with exit code {}", source.name, id, materializedPlugin.exitCode
        );
      }
    }
    return materialized;
  }

  void PluginManager::spawnMaterializeEnabled(
      PluginSourceConfig source, std::filesystem::path repoRoot, std::vector<std::string> enabled, bool cloneFirst
  ) const {
    // `this` is an Application member and outlives the worker; the registry rescan and
    // bar rebuild marshal back to the main thread via DeferredCall.
    std::thread([this, source = std::move(source), repoRoot = std::move(repoRoot), enabled = std::move(enabled),
                 cloneFirst]() mutable {
      auto sourceLock = plugin_source_locks::acquire(source.name);
      if (cloneFirst) {
        kLog.info("re-cloning missing plugin source '{}'", source.name);
        const auto cloned = plugin_git::cloneBlobless(source.location, repoRoot);
        if (!cloned) {
          if (cloned.timedOut) {
            kLog.warn("plugin source '{}': clone timed out", source.name);
          } else {
            kLog.warn("plugin source '{}': clone failed with exit code {}", source.name, cloned.exitCode);
          }
          return; // offline / unreachable — list/enable will retry
        }
      }
      if (!materializeEnabledFromRepo(source, repoRoot, enabled)) {
        return; // nothing exported; the startup registry scan already reflects disk state
      }
      DeferredCall::callLater([this]() {
        PluginRegistry::instance().scan(); // pick up the freshly exported plugins
        if (m_onChanged) {
          m_onChanged(); // rebuild bar + reconcile services
        }
      });
    }).detach();
  }

  void PluginManager::refresh() {
    const PluginsConfig& pc = m_config.config().plugins;
    if (m_applied && pc == m_lastApplied) {
      return;
    }
    // Heal wiped source storage / restored config once at startup. Source storage
    // does not change on later config reloads, so don't re-touch the network then.
    if (!m_applied) {
      ensureEnabledMaterialized(pc);
    }

    applyPluginSourcesToRegistry(PluginRegistry::instance(), pc);

    m_lastApplied = pc;
    m_applied = true;
  }

  EnableResult PluginManager::enable(std::string_view pluginId) {
    const std::string id(pluginId);
    if (!isValidPluginId(id)) {
      return {.ok = false, .error = "invalid plugin id '" + id + "' (expected author/plugin)"};
    }
    const auto subdir = pluginSubdirFromId(id);
    if (!subdir.has_value()) {
      return {.ok = false, .error = "invalid plugin id '" + id + "' (expected author/plugin)"};
    }

    // Resolving the offering source can clone a git source's catalog, and a git export
    // lazy-fetches blobs from the blobless clone — both network-bound. Do ALL of it on a
    // worker so the UI thread never blocks; isEnabling(id) drives the row spinner until
    // it lands, then persist + refresh on the main thread. Path / local-dev plugins go
    // through the same worker (fast, no network).
    if (!m_enabling.insert(id).second) {
      return {.ok = true, .error = {}}; // already in flight
    }
    if (m_onEnablingChanged) {
      m_onEnablingChanged();
    }

    auto sources = m_config.config().plugins.sources;
    std::thread([this, id, subdir = *subdir, sources = std::move(sources)]() mutable {
      const auto started = std::chrono::steady_clock::now();
      // Highest precedence wins; skip disabled sources — the registry never scans them.
      struct Offering {
        PluginSourceConfig source;
        CatalogEntry entry;
      };
      std::optional<Offering> offering;
      for (const auto& source : std::views::reverse(sources)) {
        if (!source.enabled) {
          continue;
        }
        const auto catalog = discoverCatalog(source, CatalogAccess::Network);
        if (const auto* entry = findCatalogEntry(catalog.entries, id); entry != nullptr) {
          offering = Offering{.source = source, .entry = *entry};
          break;
        }
      }

      bool ok = false;
      bool incompatible = false;
      bool timedOut = false;
      std::uint32_t pluginApiVersion = 0;
      std::string error;

      if (offering.has_value() && offering->source.kind == PluginSourceKind::Git) {
        if (offering->entry.resolvedRevision.empty()) {
          error = "source '" + offering->source.name + "' did not resolve a catalog revision";
        } else {
          auto sourceLock = plugin_source_locks::acquire(offering->source.name);
          logHeldBack(offering->source, offering->entry);
          auto materialized = materializeGitPlugin(
              offering->source, plugin_paths::gitRepoRoot(offering->source), offering->entry.resolvedRevision, id, true
          );
          ok = materialized && materialized.manifest.id == id;
          incompatible = materialized.incompatible;
          timedOut = materialized.timedOut;
          pluginApiVersion = materialized.pluginApiVersion;
          error = std::move(materialized.error);
        }
      } else if (offering.has_value()) {
        const auto manifest = parsePluginManifest(sourceRootFor(offering->source) / subdir / "plugin.toml", &error);
        if (manifest.has_value() && manifest->id != id) {
          error = "manifest id '" + manifest->id + "' does not match requested id";
        } else if (manifest.has_value() && !supportsPluginApiVersion(manifest->pluginApiVersion)) {
          incompatible = true;
          pluginApiVersion = manifest->pluginApiVersion;
        } else {
          ok = manifest.has_value();
        }
      } else if (localPluginIds().contains(id)) {
        ok = true; // implicit local dev plugin, already on disk
      } else {
        error = "no plugin '" + id + "' found in any source";
      }

      const double elapsedMs =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

      DeferredCall::callLater([this, id, ok, incompatible, timedOut, elapsedMs, pluginApiVersion,
                               error = std::move(error)]() mutable {
        m_enabling.erase(id);
        if (ok) {
          kLog.info("enabling plugin '{}' (resolved + exported in {:.0f}ms)", id, elapsedMs);
          m_config.setPluginEnabled(id, true);
          refresh();
        } else if (incompatible) {
          kLog.warn(
              "cannot enable '{}': targets plugin API {} (supported range {}-{})", id, pluginApiVersion,
              kOldestSupportedPluginApiVersion, kCurrentPluginApiVersion
          );
          notify::error(
              "Noctalia", i18n::tr("plugins.enable-failed.title"),
              i18n::tr(
                  "plugins.enable-failed.body-incompatible", "plugin", id, "version", pluginApiVersion, "oldest",
                  kOldestSupportedPluginApiVersion, "current", kCurrentPluginApiVersion
              )
          );
        } else if (timedOut) {
          kLog.warn("cannot enable '{}': export timed out", id);
          notify::error(
              "Noctalia", i18n::tr("plugins.enable-failed.title"),
              i18n::tr("plugins.enable-failed.body-timeout", "plugin", id)
          );
        } else {
          const std::string reason = error.empty() ? "export failed" : error;
          kLog.warn("cannot enable '{}': {}", id, reason);
          notify::error(
              "Noctalia", i18n::tr("plugins.enable-failed.title"),
              i18n::tr("plugins.enable-failed.body-error", "plugin", id, "error", reason)
          );
        }
        if (m_onEnablingChanged) {
          m_onEnablingChanged();
        }
      });
    }).detach();
    return {.ok = true, .error = {}};
  }

  bool PluginManager::isEnabling(std::string_view pluginId) const { return m_enabling.contains(std::string(pluginId)); }

  void PluginManager::disable(std::string_view pluginId) {
    kLog.info("disabling plugin '{}'", pluginId);
    m_config.setPluginEnabled(pluginId, false);
    refresh();
  }

  void PluginManager::remove(std::string_view pluginId) {
    const auto subdir = pluginSubdirFromId(pluginId);
    if (!subdir.has_value()) {
      return;
    }
    kLog.info("removing plugin '{}'", pluginId);
    m_config.setPluginEnabled(pluginId, false);

    const auto& plugins = m_config.config().plugins;
    for (const auto& source : plugins.sources) {
      if (!source.enabled || source.kind != PluginSourceKind::Git) {
        continue;
      }
      const auto materializedRoot = plugin_paths::gitMaterializedRoot(source);
      if (materializedRoot.empty()) {
        continue;
      }
      const auto pluginDir = materializedRoot / *subdir;
      if (std::filesystem::exists(pluginDir)) {
        auto sourceLock = plugin_source_locks::acquire(source.name);
        (void)plugin_paths::removeTreeUnder(pluginDir, materializedRoot);
      }
    }
    refresh();
  }

  std::vector<PluginStatus> PluginManager::list(CatalogAccess access) const {
    return list(m_config.config().plugins, access);
  }

  std::vector<PluginStatus> PluginManager::list(const PluginsConfig& plugins, CatalogAccess access) const {
    const std::unordered_set<std::string> enabledSet(plugins.enabled.begin(), plugins.enabled.end());

    std::vector<PluginStatus> out;
    // A plugin id is canonical: if the same id ships from more than one source, only
    // the highest-precedence copy runs (local data dir > later user source > earlier
    // > built-in default), so the catalog shows one row per id from that same source.
    // Visit highest precedence first and keep the first seen; the GUI re-sorts anyway.
    std::unordered_set<std::string> seen;
    const auto collect = [&](const std::string& sourceName, const CatalogResult& catalog,
                             const PluginSourceConfig& source) {
      for (const auto& entry : catalog.entries) {
        if (!seen.insert(entry.id).second) {
          continue;
        }
        bool onDisk = source.kind == PluginSourceKind::Path;
        if (source.kind == PluginSourceKind::Git) {
          const auto subdir = pluginSubdirFromId(entry.id);
          if (subdir.has_value()) {
            const auto root = plugin_paths::gitMaterializedRoot(source);
            onDisk = !root.empty() && std::filesystem::exists(root / *subdir);
          }
        }
        const bool isEnabled = enabledSet.contains(entry.id);
        // For git sources, `entry` reflects the fetched catalog (FETCH_HEAD); an
        // enabled+exported plugin whose on-disk manifest no longer matches it has a
        // newer release waiting to be applied via update().
        const bool updateAvailable = source.kind == PluginSourceKind::Git
            && isEnabled
            && onDisk
            && entry.compatible
            && !materializedPluginMatchesCatalog(source, entry.id, entry);
        // Show the installed version on the row; the resolved version is the target we'd
        // update to. Not-yet-installed rows have no exported copy, so fall back to the
        // resolved version.
        std::string installedVersion;
        if (source.kind == PluginSourceKind::Git && onDisk) {
          installedVersion = materializedPluginVersion(source, entry.id);
        }
        out.push_back(
            PluginStatus{
                .id = entry.id,
                .name = entry.name,
                .version = installedVersion.empty() ? entry.resolvedVersion : installedVersion,
                .availableVersion = updateAvailable ? entry.resolvedVersion : std::string{},
                .icon = entry.icon,
                .description = entry.description,
                .license = entry.license,
                .dependencies = entry.dependencies,
                .source = sourceName,
                .compatible = entry.compatible,
                .deprecated = entry.deprecated,
                .enabled = isEnabled,
                .materialized = onDisk,
                .updateAvailable = updateAvailable,
                .heldBack = entry.heldBack,
                .latestVersion = entry.heldBack ? entry.version : std::string{},
                .latestPluginApiVersion = entry.heldBack ? entry.pluginApiVersion : 0,
            }
        );
      }
    };

    if (const std::string data = FileUtils::dataDir(); !data.empty()) {
      PluginSourceConfig localSource{
          .kind = PluginSourceKind::Path,
          .name = "local",
          .location = (std::filesystem::path(data) / "plugins").string()
      };
      collect("local", discoverCatalog(localSource, access), localSource);
    }
    // Reverse config order: a later user source outranks earlier ones and the defaults.
    for (const auto& source : std::views::reverse(plugins.sources)) {
      if (!source.enabled) {
        continue;
      }
      collect(source.name, discoverCatalog(source, access), source);
    }
    return out;
  }

  void PluginManager::addSource(const PluginSourceConfig& source) {
    if (!isValidPluginSourceName(source.name)) {
      kLog.warn("refusing plugin source with invalid name '{}'", source.name);
      return;
    }
    kLog.info("adding plugin source '{}' ({})", source.name, source.location);
    if (const auto previous = findSource(source.name);
        previous.has_value() && sourceReplacementInvalidatesGitStorage(*previous, source)) {
      auto sourceLock = plugin_source_locks::acquire(source.name);
      kLog.info("plugin source '{}' changed; deleting app-managed git storage", source.name);
      removeManagedGitSourceStorage(source);
    }
    m_config.addPluginSource(source); // fires reload -> refresh re-injects the registry
  }

  void PluginManager::update(std::string sourceName) {
    const auto source = findSource(sourceName);
    if (!source.has_value() || source->kind != PluginSourceKind::Git) {
      return; // path / unknown sources are externally owned
    }
    const std::filesystem::path repoRoot = plugin_paths::gitRepoRoot(*source);
    if (repoRoot.empty()) {
      return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(repoRoot / ".git", ec)) {
      return; // nothing cloned yet
    }
    // Snapshot the enabled set for the worker (config is read on the main thread only).
    std::unordered_set<std::string> enabled;
    for (const auto& id : m_config.config().plugins.enabled) {
      if (isValidPluginId(id)) {
        enabled.insert(id);
      }
    }

    // The whole git sequence runs off-thread; only the final registry rescan marshals
    // back to the main thread. `this` is an Application member, so it outlives the worker.
    std::thread([this, source = *source, repoRoot, sourceName = std::move(sourceName),
                 enabled = std::move(enabled)]() mutable {
      auto sourceLock = plugin_source_locks::acquire(source.name);
      const auto fetched = plugin_git::fetch(repoRoot);
      if (!fetched) {
        DeferredCall::callLater([sourceName, err = fetched.err]() {
          kLog.warn("update '{}': fetch failed: {}", sourceName, err);
        });
        return;
      }
      const auto fetchedRevision = plugin_git::remoteHead(repoRoot);
      if (!fetchedRevision || fetchedRevision.out.empty()) {
        DeferredCall::callLater([sourceName, err = fetchedRevision.err]() {
          kLog.warn("update '{}': cannot resolve fetched revision: {}", sourceName, err);
        });
        return;
      }
      const auto currentRevision = plugin_git::headRevision(repoRoot);
      if (!currentRevision || currentRevision.out.empty()) {
        DeferredCall::callLater([sourceName, err = currentRevision.err]() {
          kLog.warn("update '{}': cannot resolve applied revision: {}", sourceName, err);
        });
        return;
      }
      const std::string newRev = fetchedRevision.out;
      const std::string curRev = currentRevision.out;
      const bool sourceRevisionChanged = newRev != curRev;

      const auto catalogResult = readGitCatalog(repoRoot, newRev);
      if (!catalogResult.ok) {
        DeferredCall::callLater([sourceName, err = catalogResult.error]() {
          kLog.warn("update '{}': cannot read fetched catalog: {}", sourceName, err);
        });
        return;
      }
      const auto& catalog = catalogResult.entries;
      std::unordered_set<std::string> withheldIds;
      std::vector<std::pair<std::string, std::uint32_t>> withheldUpdates;
      const auto rememberWithheld = [&](std::string id, std::uint32_t pluginApiVersion) {
        if (withheldIds.insert(id).second) {
          withheldUpdates.emplace_back(std::move(id), pluginApiVersion);
        }
      };
      std::vector<std::pair<std::string, std::string>> exportFailures;

      bool materialized = false;
      for (const auto& id : enabled) {
        const auto sub = pluginSubdirFromId(id);
        if (!sub.has_value()) {
          continue;
        }
        const auto* catalogEntry = findCatalogEntry(catalog, id);
        if (catalogEntry == nullptr) {
          continue; // this exact canonical id is not owned by this source
        }
        if (!catalogEntry->compatible) {
          rememberWithheld(id, catalogEntry->pluginApiVersion);
          continue;
        }
        if (!plugin_git::hasPath(repoRoot, *sub + "/plugin.toml", catalogEntry->resolvedRevision)) {
          exportFailures.emplace_back(id, "catalog entry has no " + *sub + "/plugin.toml");
          continue;
        }
        // A moved source tip re-exports tip-resolved plugins even when the version is
        // unchanged, so a fix that skipped a version bump still lands. A held-back plugin
        // resolves to a fixed historical revision that the tip moving does not touch, so
        // there the exported copy alone decides.
        if ((catalogEntry->heldBack || !sourceRevisionChanged)
            && materializedPluginMatchesCatalog(source, id, *catalogEntry)) {
          continue;
        }
        logHeldBack(source, *catalogEntry);
        if (const auto m = materializeGitPlugin(source, repoRoot, catalogEntry->resolvedRevision, id, true); !m) {
          if (m.incompatible) {
            rememberWithheld(id, m.pluginApiVersion);
            continue;
          }
          exportFailures.emplace_back(id, m.error);
          continue;
        }
        materialized = true;
      }
      bool applied = true;
      std::string applyError;
      if (sourceRevisionChanged) {
        const auto result = plugin_git::setHead(repoRoot, newRev);
        applied = static_cast<bool>(result);
        applyError = result.err;
      }
      DeferredCall::callLater([this, sourceName, ok = applied, err = std::move(applyError), newRev, curRev,
                               sourceRevisionChanged, materialized, exportFailures = std::move(exportFailures),
                               withheldUpdates = std::move(withheldUpdates)]() mutable {
        for (const auto& [id, exportError] : exportFailures) {
          kLog.warn("update '{}': export '{}' failed: {}", sourceName, id, exportError);
        }
        if (!ok) {
          kLog.warn("update '{}': set HEAD failed: {}", sourceName, err);
          if (materialized) {
            PluginRegistry::instance().scan();
            if (m_onChanged) {
              m_onChanged();
            }
          }
          return;
        }
        for (const auto& [id, pluginApiVersion] : withheldUpdates) {
          kLog.warn(
              "update '{}': kept previous '{}' export; new version targets plugin API {} (supported range {}-{})",
              sourceName, id, pluginApiVersion, kOldestSupportedPluginApiVersion, kCurrentPluginApiVersion
          );
        }
        if (!exportFailures.empty()) {
          if (sourceRevisionChanged) {
            kLog.warn(
                "update '{}': advanced from {} to {} while keeping {} previous export(s); retrying on the next update",
                sourceName, curRev, newRev, exportFailures.size()
            );
          } else {
            kLog.warn(
                "update '{}': {} export(s) still need reconciliation at {}; retrying on the next update", sourceName,
                exportFailures.size(), newRev
            );
          }
        }
        if (sourceRevisionChanged) {
          kLog.info("updated source '{}' -> {}", sourceName, newRev);
          if (m_onSourceUpdated) {
            m_onSourceUpdated(sourceName); // stale store thumbnails/READMEs re-fetch at the new HEAD
          }
        } else if (materialized) {
          kLog.info("reconciled source '{}' at {}", sourceName, newRev);
        } else if (exportFailures.empty()) {
          kLog.info("source '{}' already up to date", sourceName);
        }
        if (sourceRevisionChanged || materialized) {
          PluginRegistry::instance().scan(); // re-parse manifests; live .luau changes hot-reload via file watch
          if (m_onChanged) {
            m_onChanged(); // rebuild bar + reconcile services for the new revision
          }
        }
      });
    }).detach();
  }

  void PluginManager::fetchStaleCatalogs(const PluginsConfig& plugins) {
    // Newly published plugins only become browsable after a fetch advances FETCH_HEAD;
    // throttle so repeated store opens don't hit the network every time.
    constexpr auto kThrottle = std::chrono::minutes(15);
    const auto now = std::chrono::steady_clock::now();
    std::error_code ec;
    for (const auto& source : plugins.sources) {
      if (source.kind != PluginSourceKind::Git || !source.enabled) {
        continue;
      }
      {
        const std::scoped_lock lock(m_browseFetchMutex);
        const auto it = m_lastBrowseFetch.find(source.name);
        if (it != m_lastBrowseFetch.end() && now - it->second < kThrottle) {
          continue;
        }
        m_lastBrowseFetch[source.name] = now;
      }
      const std::filesystem::path repoRoot = plugin_paths::gitRepoRoot(source);
      if (repoRoot.empty() || !std::filesystem::exists(repoRoot / ".git", ec)) {
        continue; // nothing cloned yet; discoverCatalog clones on first browse
      }
      auto sourceLock = plugin_source_locks::acquire(source.name);
      if (const auto fetched = plugin_git::fetch(repoRoot); !fetched) {
        kLog.warn("browse fetch '{}' failed: {}", source.name, fetched.err);
      }
    }
  }

  void PluginManager::updateAll() {
    for (const auto& source : m_config.config().plugins.sources) {
      if (source.kind == PluginSourceKind::Git && source.enabled) {
        update(source.name);
      }
    }
  }

  void PluginManager::setAutoUpdateEnabled(bool enabled) { m_config.setPluginsAutoUpdate(enabled); }

  void PluginManager::removeSource(std::string sourceName) {
    if (isDefaultPluginSourceName(sourceName)) {
      kLog.warn("refusing to remove built-in plugin source '{}'", sourceName);
      return;
    }
    const auto source = findSource(sourceName);
    if (!source.has_value()) {
      return;
    }
    kLog.info("removing plugin source '{}'", sourceName);
    std::optional<plugin_source_locks::SourceLock> sourceLock;
    if (source->kind == PluginSourceKind::Git) {
      sourceLock.emplace(plugin_source_locks::acquire(source->name));
    }

    // Disable this source's plugins so no stale enabled ids linger. Local-only read:
    // removal runs on the main thread, and enumerating a source just to delete it must
    // not clone or lazy-fetch. A blob missing locally leaves its id enabled, same as
    // removing while offline.
    for (const auto& entry : discoverCatalog(*source, CatalogAccess::LocalOnly).entries) {
      m_config.setPluginEnabled(entry.id, false);
    }
    if (source->kind == PluginSourceKind::Git) {
      removeManagedGitSourceStorage(*source);
    }
    m_config.removePluginSource(sourceName); // fires reload -> refresh re-injects
  }

} // namespace scripting
