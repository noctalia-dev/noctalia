#pragma once

#include "scripting/plugin_manifest.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace scripting {

  // A single entry resolved against the catalog: its owning manifest, the entry
  // declaration, and the absolute path to its `.luau` source.
  struct ResolvedPluginEntry {
    const PluginManifest* manifest = nullptr;
    const PluginEntry* entry = nullptr;
    std::filesystem::path sourcePath;

    [[nodiscard]] std::string fullId() const; // "author/plugin:entry"
  };

  // Scans the user plugin directory and exposes its entries by id. P0: every
  // discovered plugin is implicitly active; distribution (git sources,
  // enable/disable) is a later phase.
  class PluginRegistry {
  public:
    // Process-wide registry. Plugins are global app state; both the widget
    // factory and the settings GUI read the same catalog.
    static PluginRegistry& instance();

    // Scan once on first use (idempotent). Call scan() directly to force a rescan.
    void ensureScanned();

    // Rescan all configured source roots (or the implicit local data dir when none
    // are set). Pointers from prior resolve()/entriesOfKind() calls are invalidated.
    void scan();

    // Source roots to scan, each a directory of `<plugin>/plugin.toml` subdirs
    // (exported git-source runtime files or a path source). Replaces the implicit
    // local data dir.
    // Forces a rescan on next ensureScanned().
    void setSources(std::vector<std::filesystem::path> sourceRoots);

    // Restrict active plugins to this set of ids ("author/plugin"). nullopt (the
    // default) activates every discovered plugin — dev/local behavior. Forces a
    // rescan on next ensureScanned().
    void setEnabledFilter(std::optional<std::unordered_set<std::string>> enabled);

    // Resolve "author/plugin:entry" to its manifest, entry, and source path.
    [[nodiscard]] std::optional<ResolvedPluginEntry> resolve(std::string_view fullEntryId) const;

    // Whether `id` names a registered entry ("author/plugin:entry").
    [[nodiscard]] bool hasEntry(std::string_view fullEntryId) const;

    // All entries of one kind (e.g. every [[widget]]) across active plugins.
    [[nodiscard]] std::vector<ResolvedPluginEntry> entriesOfKind(PluginEntryKind kind) const;

    // The manifest of a loaded plugin by its id ("author/plugin"), or null.
    [[nodiscard]] const PluginManifest* findManifest(std::string_view pluginId) const;
    [[nodiscard]] std::optional<std::filesystem::path> findPluginDir(std::string_view pluginId) const;

  private:
    struct LoadedPlugin {
      PluginManifest manifest;
      std::filesystem::path dir;
    };

    void scanDir(const std::filesystem::path& dir);
    [[nodiscard]] const LoadedPlugin* findPlugin(std::string_view pluginId) const;

    std::vector<LoadedPlugin> m_plugins;
    std::vector<std::filesystem::path> m_sourceRoots;
    std::optional<std::unordered_set<std::string>> m_enabledFilter;
    bool m_scanned = false;
  };

} // namespace scripting
