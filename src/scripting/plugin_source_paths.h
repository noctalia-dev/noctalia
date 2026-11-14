#pragma once

#include <filesystem>
#include <string>

struct PluginSourceConfig;

namespace scripting::plugin_paths {

  // Git-source repo caches. Host-managed and re-fetchable, so they live under the state dir.
  std::string pluginSourcesDir();
  // Exported runtime files for enabled git-source plugins; path sources do not use it.
  std::string pluginMaterializedDir();
  // Persistent per-plugin data directory. Survives plugin updates and honors NOCTALIA_STATE_HOME.
  // Caller creates it. Empty if no state home resolves.
  std::string pluginDataDir(const std::string& pluginId);

  std::filesystem::path localSourceRoot();
  std::filesystem::path sourceStorageRoot(const PluginSourceConfig& source);
  std::filesystem::path gitRepoRoot(const PluginSourceConfig& source);
  std::filesystem::path gitMaterializedRoot(const PluginSourceConfig& source);
  std::filesystem::path registryRoot(const PluginSourceConfig& source);

  bool pathIsInside(const std::filesystem::path& path, const std::filesystem::path& parent);
  bool removeTreeUnder(const std::filesystem::path& path, const std::filesystem::path& parent);

} // namespace scripting::plugin_paths
