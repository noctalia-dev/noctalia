#pragma once

#include <filesystem>

struct PluginSourceConfig;

namespace scripting::plugin_paths {

  [[nodiscard]] std::filesystem::path localSourceRoot();
  [[nodiscard]] std::filesystem::path sourceStorageRoot(const PluginSourceConfig& source);
  [[nodiscard]] std::filesystem::path gitRepoRoot(const PluginSourceConfig& source);
  [[nodiscard]] std::filesystem::path gitMaterializedRoot(const PluginSourceConfig& source);
  [[nodiscard]] std::filesystem::path registryRoot(const PluginSourceConfig& source);

  [[nodiscard]] bool pathIsInside(const std::filesystem::path& path, const std::filesystem::path& parent);
  bool removeTreeUnder(const std::filesystem::path& path, const std::filesystem::path& parent);

} // namespace scripting::plugin_paths
