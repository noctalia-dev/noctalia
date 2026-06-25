#include "scripting/plugin_source_paths.h"

#include "config/config_types.h"
#include "util/file_utils.h"

#include <system_error>

namespace scripting::plugin_paths {

  namespace {
    std::filesystem::path normalizedAbsolute(const std::filesystem::path& path) {
      if (path.empty()) {
        return {};
      }
      std::error_code ec;
      auto absolute = std::filesystem::absolute(path, ec);
      if (ec) {
        absolute = path;
      }
      return absolute.lexically_normal();
    }
  } // namespace

  std::filesystem::path localSourceRoot() {
    const std::string data = FileUtils::dataDir();
    if (data.empty()) {
      return {};
    }
    return std::filesystem::path(data) / "plugins";
  }

  std::filesystem::path sourceStorageRoot(const PluginSourceConfig& source) {
    if (!isValidPluginSourceName(source.name)) {
      return {};
    }
    const std::string base = FileUtils::pluginSourcesDir();
    if (base.empty()) {
      return {};
    }
    return std::filesystem::path(base) / source.name;
  }

  std::filesystem::path gitRepoRoot(const PluginSourceConfig& source) {
    const auto root = sourceStorageRoot(source);
    return root.empty() ? std::filesystem::path{} : root / "repo";
  }

  std::filesystem::path gitMaterializedRoot(const PluginSourceConfig& source) {
    if (!isValidPluginSourceName(source.name)) {
      return {};
    }
    const std::string base = FileUtils::pluginMaterializedDir();
    if (base.empty()) {
      return {};
    }
    return std::filesystem::path(base) / source.name;
  }

  std::filesystem::path registryRoot(const PluginSourceConfig& source) {
    if (!isValidPluginSourceName(source.name)) {
      return {};
    }
    if (source.kind == PluginSourceKind::Path) {
      return FileUtils::expandUserPath(source.location);
    }
    return gitMaterializedRoot(source);
  }

  bool pathIsInside(const std::filesystem::path& path, const std::filesystem::path& parent) {
    const auto child = normalizedAbsolute(path);
    const auto root = normalizedAbsolute(parent);
    if (child.empty() || root.empty() || child == root) {
      return false;
    }

    auto childIt = child.begin();
    auto rootIt = root.begin();
    for (; rootIt != root.end(); ++rootIt, ++childIt) {
      if (childIt == child.end() || *childIt != *rootIt) {
        return false;
      }
    }
    return true;
  }

  bool removeTreeUnder(const std::filesystem::path& path, const std::filesystem::path& parent) {
    if (!pathIsInside(path, parent)) {
      return false;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return !ec;
  }

} // namespace scripting::plugin_paths
