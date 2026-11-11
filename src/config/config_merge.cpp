#include "config/config_merge.h"

#include "config/config_service.h"
#include "core/application_identity.h"
#include "core/log.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <ranges>
#include <set>
#include <system_error>
#include <utility>

namespace noctalia::config {

  namespace {

    constexpr Logger kLog("config");

    // Sorted *.toml directly in `dir` (non-recursive). Mirrors the root-scan in
    // ConfigService so directory includes load in a stable, predictable order.
    std::vector<std::filesystem::path> sortedTomlInDir(const std::filesystem::path& dir) {
      std::vector<std::filesystem::path> files;
      std::error_code ec;
      if (!std::filesystem::is_directory(dir, ec) || ec) {
        return files;
      }
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".toml") {
          files.push_back(entry.path());
        }
      }
      std::ranges::sort(files);
      return files;
    }

    std::filesystem::path canonicalKey(const std::filesystem::path& path) {
      std::error_code ec;
      auto key = std::filesystem::weakly_canonical(path, ec);
      return ec ? path.lexically_normal() : key;
    }

    struct IncludeEntry {
      std::string value;
      schema::SourceOrigin origin;
    };

    struct IncludeDirective {
      std::vector<IncludeEntry> files;
      bool autoload = true;
      bool hasAutoload = false;
    };

    IncludeDirective readInclude(const std::filesystem::path& path, const toml::table& tbl) {
      IncludeDirective directive;
      const auto* inc = tbl["include"].as_table();
      if (inc == nullptr) {
        return directive;
      }
      if (auto v = (*inc)["autoload"].value<bool>()) {
        directive.autoload = *v;
        directive.hasAutoload = true;
      }
      if (const auto* arr = (*inc)["files"].as_array()) {
        for (const auto& node : *arr) {
          if (auto s = node.value<std::string>()) {
            const auto& src = node.source();
            directive.files.push_back(
                IncludeEntry{std::move(*s), schema::SourceOrigin{path.string(), src.begin.line, src.begin.column}}
            );
          }
        }
      }
      return directive;
    }

    // Forward declaration: parse `path`, then expand it (mutual recursion with the
    // include loop below).
    toml::table
    loadAndExpand(const std::filesystem::path& path, std::set<std::filesystem::path>& visited, MergeResult& out);

    // Expands an already-parsed file: merges its includes first, then overlays the
    // file's own body (minus [include]) so the host file wins. Returns the merged
    // subtree contributed by this file.
    toml::table expandFile(
        const std::filesystem::path& path, const toml::table& parsed, std::set<std::filesystem::path>& visited,
        MergeResult& out
    ) {
      const auto key = canonicalKey(path);
      if (visited.contains(key)) {
        kLog.warn("config include cycle or duplicate skipped: {}", key.string());
        return toml::table{};
      }
      visited.insert(key);
      out.loadedFiles.push_back(key);

      const std::string includingDir = path.parent_path().string();
      const IncludeDirective directive = readInclude(path, parsed);

      toml::table base;
      for (const auto& entry : directive.files) {
        const std::string expanded = FileUtils::expandEnvVars(entry.value);
        const std::filesystem::path target = FileUtils::resolvePath(expanded, includingDir);

        std::error_code ec;
        if (std::filesystem::is_directory(target, ec) && !ec) {
          out.watchedDirs.push_back(canonicalKey(target));
          for (const auto& child : sortedTomlInDir(target)) {
            ConfigService::deepMerge(base, loadAndExpand(child, visited, out));
          }
        } else if (std::filesystem::is_regular_file(target, ec) && !ec) {
          ConfigService::deepMerge(base, loadAndExpand(target, visited, out));
        } else {
          if (out.firstError.empty()) {
            out.firstError = std::format("include not found: {}", entry.value);
            out.firstErrorOrigin = entry.origin;
          }
          kLog.warn("config include not found: {} (from {})", target.string(), path.string());
        }
      }

      // Recorded after the includes so the host file's keys win here too.
      out.origins.record(path, parsed);

      // Host wins: the file's own body overlays the includes it pulled in.
      toml::table body = parsed;
      body.erase("include");
      ConfigService::deepMerge(base, body);
      return base;
    }

    toml::table
    loadAndExpand(const std::filesystem::path& path, std::set<std::filesystem::path>& visited, MergeResult& out) {
      toml::table parsed;
      try {
        parsed = toml::parse_file(path.string());
      } catch (const toml::parse_error& e) {
        if (out.firstError.empty()) {
          out.firstError = std::string(e.description());
          out.firstErrorOrigin = parseErrorOrigin(e, path);
        }
        kLog.warn("parse error in {}: {}", path.string(), e.description());
        return toml::table{};
      }
      return expandFile(path, parsed, visited, out);
    }

    void mergeRootDir(const std::filesystem::path& dir, std::set<std::filesystem::path>& visited, MergeResult& out) {
      // Phase A — parse every root file, noting any [include].autoload = false.
      struct Root {
        std::filesystem::path path;
        toml::table table;
        bool optOut = false;
      };
      std::vector<Root> roots;
      bool anyOptOut = false;
      for (const auto& path : sortedTomlInDir(dir)) {
        toml::table tbl;
        try {
          tbl = toml::parse_file(path.string());
        } catch (const toml::parse_error& e) {
          if (out.firstError.empty()) {
            out.firstError = std::string(e.description());
            out.firstErrorOrigin = parseErrorOrigin(e, path);
          }
          kLog.warn("parse error in {}: {}", path.string(), e.description());
          continue;
        }
        const IncludeDirective directive = readInclude(path, tbl);
        const bool optOut = directive.hasAutoload && !directive.autoload;
        anyOptOut = anyOptOut || optOut;
        roots.push_back(Root{.path = path, .table = std::move(tbl), .optOut = optOut});
      }

      // Phase B — merge the load set. When any root opts out, only the opting-out
      // roots (and their recursive includes) are loaded; other roots are skipped.
      for (const auto& root : roots) {
        if (anyOptOut && !root.optOut) {
          continue;
        }
        ConfigService::deepMerge(out.merged, expandFile(root.path, root.table, visited, out));
      }
    }

  } // namespace

  std::vector<std::string> systemConfigDirs() {
    const char* raw = std::getenv("XDG_CONFIG_DIRS");
    const std::string_view searchPath = (raw != nullptr && raw[0] != '\0') ? std::string_view(raw) : "/etc/xdg";
    const std::string& appName = applicationIdentity().name;

    std::vector<std::string> dirs;
    for (const auto base : searchPath | std::views::split(':')) {
      const std::string_view baseDir(base.begin(), base.end());
      if (baseDir.starts_with('/')) {
        dirs.push_back(std::string(baseDir) + "/" + appName);
      }
    }
    return dirs;
  }

  MergeResult mergeConfigWithIncludes(std::string_view configDir, std::span<const std::string> systemConfigDirs) {
    MergeResult out;
    std::set<std::filesystem::path> visited;
    const std::filesystem::path userDir = configDir.empty() ? std::filesystem::path{} : canonicalKey(configDir);

    for (const auto& systemDir : systemConfigDirs | std::views::reverse) {
      const std::filesystem::path dir = canonicalKey(systemDir);
      std::error_code ec;
      if (dir == userDir || !std::filesystem::is_directory(dir, ec) || ec) {
        continue;
      }
      out.watchedDirs.push_back(dir);
      mergeRootDir(dir, visited, out);
    }

    if (!userDir.empty()) {
      mergeRootDir(std::filesystem::path(configDir), visited, out);
    }
    return out;
  }

} // namespace noctalia::config
