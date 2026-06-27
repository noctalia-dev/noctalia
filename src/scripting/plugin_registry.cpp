#include "scripting/plugin_registry.h"

#include "core/build_info.h"
#include "core/log.h"
#include "core/version.h"
#include "util/file_utils.h"

#include <algorithm>
#include <system_error>

namespace scripting {

  namespace {
    constexpr Logger kLog("plugins");
  } // namespace

  std::string ResolvedPluginEntry::fullId() const {
    if (manifest == nullptr || entry == nullptr) {
      return {};
    }
    return manifest->id + ":" + entry->id;
  }

  PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry registry;
    return registry;
  }

  void PluginRegistry::ensureScanned() {
    if (!m_scanned) {
      scan();
    }
  }

  void PluginRegistry::setSources(std::vector<std::filesystem::path> sourceRoots) {
    m_sourceRoots = std::move(sourceRoots);
    m_scanned = false;
  }

  void PluginRegistry::setEnabledFilter(std::optional<std::unordered_set<std::string>> enabled) {
    m_enabledFilter = std::move(enabled);
    m_scanned = false;
  }

  void PluginRegistry::scan() {
    m_plugins.clear();
    std::vector<std::filesystem::path> roots = m_sourceRoots;
    if (roots.empty()) {
      // Implicit local source: the user data dir (honoring NOCTALIA_DATA_HOME). When
      // the plugin manager is wired it injects the resolved git/path source roots.
      if (const std::string data = FileUtils::dataDir(); !data.empty()) {
        roots.emplace_back(std::filesystem::path(data) / "plugins");
      }
    }
    for (const auto& root : roots) {
      scanDir(root);
    }
    m_scanned = true;
  }

  void PluginRegistry::scanDir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      return;
    }

    for (const auto& sub : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) {
        break;
      }
      if (!sub.is_directory()) {
        continue;
      }
      const std::filesystem::path manifestPath = sub.path() / "plugin.toml";
      if (!std::filesystem::exists(manifestPath, ec)) {
        continue;
      }

      std::string error;
      auto manifest = parsePluginManifest(manifestPath, &error);
      if (!manifest.has_value()) {
        kLog.warn("ignoring plugin at {}: {}", sub.path().string(), error);
        continue;
      }
      if (m_enabledFilter.has_value() && !m_enabledFilter->contains(manifest->id)) {
        continue; // discovered but not enabled
      }
      if (!noctalia::version::atLeast(noctalia::build_info::version(), manifest->minNoctalia)) {
        kLog.warn(
            "ignoring plugin '{}' at {}: requires noctalia >= {} (running {})", manifest->id, sub.path().string(),
            manifest->minNoctalia, noctalia::build_info::version()
        );
        continue;
      }
      // Roots are scanned lowest-to-highest precedence, so a later copy of the same
      // canonical id overrides an earlier one (e.g. a sideloaded source shadowing a
      // built-in). One plugin runs per id — no duplicate providers/services/IPC.
      const auto existing =
          std::ranges::find_if(m_plugins, [&](const LoadedPlugin& p) { return p.manifest.id == manifest->id; });
      if (existing != m_plugins.end()) {
        kLog.info(
            "plugin '{}' at {} overrides earlier copy at {}", manifest->id, sub.path().string(), existing->dir.string()
        );
        existing->manifest = std::move(*manifest);
        existing->dir = sub.path();
        continue;
      }
      kLog.info("loaded plugin '{}' ({} entries) from {}", manifest->id, manifest->entries.size(), sub.path().string());
      m_plugins.push_back(LoadedPlugin{.manifest = std::move(*manifest), .dir = sub.path()});
    }
  }

  const PluginRegistry::LoadedPlugin* PluginRegistry::findPlugin(std::string_view pluginId) const {
    const auto it =
        std::ranges::find_if(m_plugins, [pluginId](const LoadedPlugin& p) { return p.manifest.id == pluginId; });
    return it != m_plugins.end() ? &*it : nullptr;
  }

  std::optional<ResolvedPluginEntry> PluginRegistry::resolve(std::string_view fullEntryId) const {
    const auto colon = fullEntryId.find(':');
    if (colon == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view pluginId = fullEntryId.substr(0, colon);
    const std::string_view entryId = fullEntryId.substr(colon + 1);

    const LoadedPlugin* plugin = findPlugin(pluginId);
    if (plugin == nullptr) {
      return std::nullopt;
    }
    const PluginEntry* entry = plugin->manifest.findEntry(entryId);
    if (entry == nullptr) {
      return std::nullopt;
    }
    return ResolvedPluginEntry{.manifest = &plugin->manifest, .entry = entry, .sourcePath = plugin->dir / entry->entry};
  }

  bool PluginRegistry::hasEntry(std::string_view fullEntryId) const { return resolve(fullEntryId).has_value(); }

  std::vector<ResolvedPluginEntry> PluginRegistry::entriesOfKind(PluginEntryKind kind) const {
    std::vector<ResolvedPluginEntry> out;
    for (const LoadedPlugin& plugin : m_plugins) {
      for (const PluginEntry& entry : plugin.manifest.entries) {
        if (entry.kind == kind) {
          out.push_back(
              ResolvedPluginEntry{.manifest = &plugin.manifest, .entry = &entry, .sourcePath = plugin.dir / entry.entry}
          );
        }
      }
    }
    return out;
  }

  const PluginManifest* PluginRegistry::findManifest(std::string_view pluginId) const {
    const LoadedPlugin* plugin = findPlugin(pluginId);
    return plugin != nullptr ? &plugin->manifest : nullptr;
  }

  std::optional<std::filesystem::path> PluginRegistry::findPluginDir(std::string_view pluginId) const {
    const LoadedPlugin* plugin = findPlugin(pluginId);
    if (plugin == nullptr) {
      return std::nullopt;
    }
    return plugin->dir;
  }

} // namespace scripting
