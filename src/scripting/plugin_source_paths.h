#pragma once

#include <filesystem>

struct PluginSourceConfig;

namespace scripting::plugin_paths {

  std::filesystem::path localSourceRoot();
  std::filesystem::path sourceStorageRoot(const PluginSourceConfig& source);
  std::filesystem::path gitRepoRoot(const PluginSourceConfig& source);
  std::filesystem::path gitMaterializedRoot(const PluginSourceConfig& source);
  std::filesystem::path registryRoot(const PluginSourceConfig& source);

  bool pathIsInside(const std::filesystem::path& path, const std::filesystem::path& parent);
  bool removeTreeUnder(const std::filesystem::path& path, const std::filesystem::path& parent);

} // namespace scripting::plugin_paths
