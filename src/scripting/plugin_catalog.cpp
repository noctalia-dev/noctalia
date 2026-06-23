#include "scripting/plugin_catalog.h"

#include "core/build_info.h"
#include "core/log.h"
#include "core/toml.h" // IWYU pragma: keep
#include "core/version.h"
#include "scripting/plugin_git.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_source_locks.h"
#include "scripting/plugin_source_paths.h"
#include "util/file_utils.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace scripting {

  namespace {
    const Logger kLog{"plugins"};

    std::string tableString(const toml::table& tbl, std::string_view key, std::string fallback = {}) {
      return tbl[key].value<std::string>().value_or(std::move(fallback));
    }

    std::vector<std::string> tableStringArray(const toml::table& tbl, std::string_view key) {
      std::vector<std::string> out;
      const auto* values = tbl[key].as_array();
      if (values == nullptr) {
        return out;
      }
      for (const auto& node : *values) {
        if (auto value = node.value<std::string>()) {
          out.push_back(*value);
        }
      }
      return out;
    }

    bool readFileToString(const std::filesystem::path& path, std::string& out) {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return false;
      }
      std::ostringstream ss;
      ss << file.rdbuf();
      out = ss.str();
      return true;
    }

    void fillCompat(CatalogEntry& e) {
      e.compatible =
          e.minNoctalia.empty() || noctalia::version::atLeast(noctalia::build_info::version(), e.minNoctalia);
    }

    // Build a catalog row from a full manifest (path-source scan fallback).
    CatalogEntry entryFromManifest(const PluginManifest& m) {
      CatalogEntry e{
          .id = m.id,
          .name = m.name,
          .tags = m.tags,
          .dependencies = m.dependencies,
          .version = m.version,
          .author = m.author,
          .icon = m.icon,
          .description = m.description,
          .license = m.license,
          .minNoctalia = m.minNoctalia,
          .deprecated = m.deprecated,
      };
      fillCompat(e);
      return e;
    }

    // Scan a directory of plugins (each `<dir>/<plugin>/plugin.toml`).
    std::vector<CatalogEntry> scanDir(const std::filesystem::path& dir) {
      std::vector<CatalogEntry> out;
      std::error_code ec;
      for (const auto& sub : std::filesystem::directory_iterator(dir, ec)) {
        if (!sub.is_directory()) {
          continue;
        }
        const auto manifestPath = sub.path() / "plugin.toml";
        if (!std::filesystem::exists(manifestPath, ec)) {
          continue;
        }
        std::string error;
        if (auto m = parsePluginManifest(manifestPath, &error)) {
          out.push_back(entryFromManifest(*m));
        } else {
          kLog.warn("catalog scan: {}", error);
        }
      }
      return out;
    }
  } // namespace

  std::vector<CatalogEntry> parseCatalogToml(const std::string& body) {
    std::vector<CatalogEntry> out;
    toml::table root;
    try {
      root = toml::parse(body);
    } catch (const toml::parse_error& err) {
      kLog.warn("catalog parse error: {}", std::string(err.description()));
      return out;
    }

    const auto* plugins = root["plugin"].as_array();
    if (plugins == nullptr) {
      return out;
    }
    for (const auto& node : *plugins) {
      const auto* tbl = node.as_table();
      if (tbl == nullptr) {
        continue;
      }
      CatalogEntry e{
          .id = tableString(*tbl, "id"),
          .name = tableString(*tbl, "name"),
          .tags = tableStringArray(*tbl, "tags"),
          .dependencies = tableStringArray(*tbl, "dependencies"),
          .version = tableString(*tbl, "version"),
          .author = tableString(*tbl, "author"),
          .icon = tableString(*tbl, "icon"),
          .description = tableString(*tbl, "description"),
          .license = tableString(*tbl, "license", "MIT"),
          .minNoctalia = tableString(*tbl, "min_noctalia"),
          .deprecated = (*tbl)["deprecated"].value<bool>().value_or(false),
      };
      if (e.id.empty()) {
        kLog.warn("catalog row missing mandatory key 'id'");
        continue;
      }
      if (!isValidPluginId(e.id)) {
        kLog.warn("catalog row has invalid plugin id '{}'; expected author/plugin", e.id);
        continue;
      }
      if (e.name.empty()) {
        kLog.warn("catalog row '{}' missing mandatory key 'name'", e.id);
        continue;
      }
      fillCompat(e);
      out.push_back(std::move(e));
    }
    return out;
  }

  CatalogResult discoverCatalog(const PluginSourceConfig& source) {
    if (!isValidPluginSourceName(source.name)) {
      return {.ok = false, .error = "invalid plugin source name: " + source.name, .entries = {}};
    }
    if (source.kind == PluginSourceKind::Path) {
      std::error_code ec;
      const std::filesystem::path dir = FileUtils::expandUserPath(source.location);
      if (!std::filesystem::is_directory(dir, ec)) {
        return {.ok = false, .error = "path source directory not found: " + dir.string(), .entries = {}};
      }
      const auto catalogPath = dir / "catalog.toml";
      if (std::filesystem::exists(catalogPath, ec)) {
        std::string body;
        if (readFileToString(catalogPath, body)) {
          return {.ok = true, .error = {}, .entries = parseCatalogToml(body)};
        }
      }
      // No catalog.toml — path sources are on disk, so scan straight away.
      return {.ok = true, .error = {}, .entries = scanDir(dir)};
    }

    // Git source: clone-if-needed (blobless, no-checkout), then read the catalog
    // via `git show`. Runtime plugin files are exported separately on enable/update.
    if (!plugin_git::available()) {
      return {.ok = false, .error = "git is not installed", .entries = {}};
    }
    const std::filesystem::path dest = plugin_paths::gitRepoRoot(source);
    if (dest.empty()) {
      return {.ok = false, .error = "empty plugin source repo path", .entries = {}};
    }
    auto sourceLock = plugin_source_locks::acquire(source.name);
    std::error_code ec;
    if (!std::filesystem::exists(dest / ".git", ec)) {
      std::filesystem::create_directories(dest.parent_path(), ec);
      auto cloned = plugin_git::cloneBlobless(source.location, dest);
      if (!cloned) {
        return {.ok = false, .error = "clone failed: " + cloned.err, .entries = {}};
      }
    }
    auto shown = plugin_git::showFile(dest, "catalog.toml");
    if (!shown) {
      return {.ok = false, .error = "no catalog.toml in source '" + source.name + "'", .entries = {}};
    }
    return {.ok = true, .error = {}, .entries = parseCatalogToml(shown.out)};
  }

} // namespace scripting
