#include "scripting/plugin_catalog.h"

#include "config/config_types.h"
#include "core/log.h"
#include "core/toml.h" // IWYU pragma: keep
#include "scripting/plugin_api.h"
#include "scripting/plugin_git.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_source_locks.h"
#include "scripting/plugin_source_paths.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
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

    bool isCommitSha(std::string_view rev) {
      return rev.size() == 40
          && std::ranges::all_of(rev, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    // Read `plugin_api` as a positive value that fits the field, or nullopt.
    std::optional<std::uint32_t> tablePluginApiVersion(const toml::table& tbl) {
      const auto value = tbl["plugin_api"].value<std::int64_t>();
      if (!value.has_value()
          || *value <= 0
          || static_cast<std::uint64_t>(*value) > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
      }
      return static_cast<std::uint32_t>(*value);
    }

    // `[[plugin.release]]` rows: older revisions of the same plugin. Sorted by descending
    // API level, which is also newest-first because each row is the newest revision at or
    // below its level.
    std::vector<CatalogRelease> parseReleases(const toml::table& tbl, std::string_view id, std::uint32_t tipVersion) {
      std::vector<CatalogRelease> out;
      const auto* rows = tbl["release"].as_array();
      if (rows == nullptr) {
        return out;
      }
      for (const auto& node : *rows) {
        const auto* row = node.as_table();
        if (row == nullptr) {
          continue;
        }
        const auto pluginApiVersion = tablePluginApiVersion(*row);
        if (!pluginApiVersion.has_value()) {
          kLog.warn("catalog row '{}' release has invalid 'plugin_api'; expected a positive integer", id);
          continue;
        }
        if (*pluginApiVersion >= tipVersion) {
          kLog.warn(
              "catalog row '{}' release targets plugin API {}, at or above the tip's {}; ignoring", id,
              *pluginApiVersion, tipVersion
          );
          continue;
        }
        CatalogRelease release{
            .pluginApiVersion = *pluginApiVersion,
            .version = tableString(*row, "version"),
            .revision = tableString(*row, "rev"),
        };
        if (release.version.empty()) {
          kLog.warn("catalog row '{}' release for plugin API {} missing 'version'", id, release.pluginApiVersion);
          continue;
        }
        if (!isCommitSha(release.revision)) {
          kLog.warn(
              "catalog row '{}' release for plugin API {} has invalid 'rev'; expected a full commit sha", id,
              release.pluginApiVersion
          );
          continue;
        }
        out.push_back(std::move(release));
      }
      std::ranges::sort(out, std::ranges::greater{}, &CatalogRelease::pluginApiVersion);
      return out;
    }

    // Pick the revision this host installs: the tip when its API level is supported, else
    // the highest supported release. `headRevision` is empty for path sources, which have
    // no revisions to export and so are always judged on the tip alone.
    void resolveCompat(CatalogEntry& e, std::string_view headRevision) {
      e.resolvedRevision = std::string(headRevision);
      e.resolvedVersion = e.version;
      e.resolvedPluginApiVersion = e.pluginApiVersion;
      e.heldBack = false;
      e.compatible = supportsPluginApiVersion(e.pluginApiVersion);
      if (e.compatible || headRevision.empty()) {
        return;
      }
      for (const auto& release : e.releases) {
        if (!supportsPluginApiVersion(release.pluginApiVersion)) {
          continue;
        }
        e.resolvedRevision = release.revision;
        e.resolvedVersion = release.version;
        e.resolvedPluginApiVersion = release.pluginApiVersion;
        e.heldBack = true;
        e.compatible = true;
        return;
      }
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
          .pluginApiVersion = m.pluginApiVersion,
          .deprecated = m.deprecated,
      };
      resolveCompat(e, "");
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

  std::vector<CatalogEntry> parseCatalogToml(const std::string& body, std::string_view headRevision) {
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
          .updatedAt = std::chrono::system_clock::time_point{std::chrono::seconds{
              (*tbl)["updated_at"].value<std::uint64_t>().value_or(0)
          }},
          .addedAt = std::chrono::system_clock::time_point{std::chrono::seconds{
              (*tbl)["added_at"].value<std::uint64_t>().value_or(0)
          }},
          .author = tableString(*tbl, "author"),
          .icon = tableString(*tbl, "icon"),
          .description = tableString(*tbl, "description"),
          .license = tableString(*tbl, "license", "MIT"),
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
      const auto pluginApiVersion = tablePluginApiVersion(*tbl);
      if (!pluginApiVersion.has_value()) {
        kLog.warn("catalog row '{}' has invalid mandatory key 'plugin_api'; expected a positive integer", e.id);
        continue;
      }
      e.pluginApiVersion = *pluginApiVersion;
      e.releases = parseReleases(*tbl, e.id, e.pluginApiVersion);
      resolveCompat(e, headRevision);
      out.push_back(std::move(e));
    }
    return out;
  }

  CatalogResult discoverCatalog(const PluginSourceConfig& source, CatalogAccess access) {
    if (!isValidPluginSourceName(source.name)) {
      return {.ok = false, .error = "invalid plugin source name: " + source.name, .entries = {}, .revision = {}};
    }
    if (source.kind == PluginSourceKind::Path) {
      std::error_code ec;
      const std::filesystem::path dir = FileUtils::expandUserPath(source.location);
      if (!std::filesystem::is_directory(dir, ec)) {
        return {
            .ok = false, .error = "path source directory not found: " + dir.string(), .entries = {}, .revision = {}
        };
      }
      const auto catalogPath = dir / "catalog.toml";
      if (std::filesystem::exists(catalogPath, ec)) {
        std::string body;
        if (readFileToString(catalogPath, body)) {
          return {.ok = true, .error = {}, .entries = parseCatalogToml(body, ""), .revision = {}};
        }
      }
      // No catalog.toml — path sources are on disk, so scan straight away.
      return {.ok = true, .error = {}, .entries = scanDir(dir), .revision = {}};
    }

    // Git source: clone-if-needed (blobless, no-checkout), then read the catalog
    // via `git show`. Runtime plugin files are exported separately on enable/update.
    if (!plugin_git::available()) {
      return {.ok = false, .error = "git is not installed", .entries = {}, .revision = {}};
    }
    const std::filesystem::path dest = plugin_paths::gitRepoRoot(source);
    if (dest.empty()) {
      return {.ok = false, .error = "empty plugin source repo path", .entries = {}, .revision = {}};
    }
    auto sourceLock = plugin_source_locks::acquire(source.name);
    const bool localOnly = access == CatalogAccess::LocalOnly;
    std::error_code ec;
    if (!std::filesystem::exists(dest / ".git", ec)) {
      if (localOnly) {
        return {.ok = false, .error = "source '" + source.name + "' is not cloned yet", .entries = {}, .revision = {}};
      }
      std::filesystem::create_directories(dest.parent_path(), ec);
      auto cloned = plugin_git::cloneBlobless(source.location, dest);
      if (!cloned) {
        return {.ok = false, .error = "clone failed: " + cloned.err, .entries = {}, .revision = {}};
      }
    }
    // Browse the freshest catalog: when a prior fetch left FETCH_HEAD ahead of the
    // applied HEAD, read the catalog there so newly published plugins are listed.
    // Return the exact chosen revision so later file reads/exports use the same tree.
    // The fetch itself is throttled and off-thread in PluginManager::fetchStaleCatalogs.
    const auto head = plugin_git::headRevision(dest);
    if (!head || head.out.empty()) {
      return {
          .ok = false, .error = "cannot resolve HEAD for source '" + source.name + "'", .entries = {}, .revision = {}
      };
    }
    std::string rev = head.out;
    if (const auto fetched = plugin_git::remoteHead(dest); fetched && !fetched.out.empty()) {
      if (head.out != fetched.out) {
        rev = fetched.out;
      }
    }
    auto shown = plugin_git::showFile(dest, "catalog.toml", rev, localOnly);
    if (!shown && rev != head.out) {
      // Fetched blob unreachable (e.g. offline / local-only); fall back to the applied HEAD.
      rev = head.out;
      shown = plugin_git::showFile(dest, "catalog.toml", rev, localOnly);
    }
    if (!shown) {
      return {.ok = false, .error = "no catalog.toml in source '" + source.name + "'", .entries = {}, .revision = {}};
    }
    return {.ok = true, .error = {}, .entries = parseCatalogToml(shown.out, rev), .revision = std::move(rev)};
  }

} // namespace scripting
