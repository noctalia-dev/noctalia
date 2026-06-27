#include "config/config_merge.h"

#include "config/config_service.h"
#include "core/log.h"
#include "util/file_utils.h"

#include <algorithm>
#include <format>
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

    struct IncludeDirective {
      std::vector<std::string> files;
      bool autoload = true;
      bool hasAutoload = false;
    };

    IncludeDirective readInclude(const toml::table& tbl) {
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
            directive.files.push_back(*s);
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
      const IncludeDirective directive = readInclude(parsed);

      toml::table base;
      for (const auto& entry : directive.files) {
        const std::string expanded = FileUtils::expandEnvVars(entry);
        const std::filesystem::path target = FileUtils::resolvePath(expanded, includingDir);

        std::error_code ec;
        if (std::filesystem::is_directory(target, ec) && !ec) {
          out.includeDirs.push_back(canonicalKey(target));
          for (const auto& child : sortedTomlInDir(target)) {
            ConfigService::deepMerge(base, loadAndExpand(child, visited, out));
          }
        } else if (std::filesystem::is_regular_file(target, ec) && !ec) {
          ConfigService::deepMerge(base, loadAndExpand(target, visited, out));
        } else {
          if (out.firstError.empty()) {
            out.firstError = std::format("include not found: {} (from {})", entry, path.filename().string());
          }
          kLog.warn("config include not found: {} (from {})", target.string(), path.string());
        }
      }

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
        const auto& src = e.source();
        if (out.firstError.empty()) {
          out.firstError = std::format(
              "{} line {}, column {}: {}", path.filename().string(), src.begin.line, src.begin.column, e.description()
          );
        }
        kLog.warn("parse error in {}: {}", path.filename().string(), e.description());
        return toml::table{};
      }
      return expandFile(path, parsed, visited, out);
    }

  } // namespace

  MergeResult mergeConfigWithIncludes(std::string_view configDir) {
    MergeResult out;
    if (configDir.empty()) {
      return out;
    }

    // Phase A — parse every root file, noting any [include].autoload = false.
    struct Root {
      std::filesystem::path path;
      toml::table table;
      bool optOut = false;
    };
    std::vector<Root> roots;
    bool anyOptOut = false;
    for (const auto& path : sortedTomlInDir(std::filesystem::path(configDir))) {
      toml::table tbl;
      try {
        tbl = toml::parse_file(path.string());
      } catch (const toml::parse_error& e) {
        const auto& src = e.source();
        if (out.firstError.empty()) {
          out.firstError = std::format(
              "{} line {}, column {}: {}", path.filename().string(), src.begin.line, src.begin.column, e.description()
          );
        }
        kLog.warn("parse error in {}: {}", path.filename().string(), e.description());
        continue;
      }
      const IncludeDirective directive = readInclude(tbl);
      const bool optOut = directive.hasAutoload && !directive.autoload;
      anyOptOut = anyOptOut || optOut;
      roots.push_back(Root{.path = path, .table = std::move(tbl), .optOut = optOut});
    }

    // Phase B — merge the load set. When any root opts out, only the opting-out
    // roots (and their recursive includes) are loaded; other roots are skipped.
    std::set<std::filesystem::path> visited;
    for (const auto& root : roots) {
      if (anyOptOut && !root.optOut) {
        continue;
      }
      ConfigService::deepMerge(out.merged, expandFile(root.path, root.table, visited, out));
    }

    return out;
  }

} // namespace noctalia::config
