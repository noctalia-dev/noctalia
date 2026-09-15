#include "config/config_merge.h"

#include "config/config_service.h"
#include "core/log.h"
#include "util/file_utils.h"

#include <algorithm>
#include <format>
#include <set>
#include <sstream>
#include <system_error>
#include <type_traits>
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
          out.includeDirs.push_back(canonicalKey(target));
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

    // Canonical textual form of a scalar/array leaf, used only for equality
    // comparison between the two merge layers (formatting quirks do not matter
    // as long as both sides go through the same serializer).
    void appendLeafValue(const toml::node& node, std::string& out) {
      node.visit([&out](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, toml::table>) {
          out += "<table>";
        } else if constexpr (std::is_same_v<Value, toml::array>) {
          out += '[';
          bool first = true;
          for (const auto& element : value) {
            if (!first) {
              out += ',';
            }
            first = false;
            appendLeafValue(element, out);
          }
          out += ']';
        } else {
          std::ostringstream stream;
          stream << value;
          out += stream.str();
        }
      });
    }

    [[nodiscard]] bool leafValuesDiffer(const toml::node& base, const toml::node& overlay) {
      std::string baseValue;
      std::string overlayValue;
      appendLeafValue(base, baseValue);
      appendLeafValue(overlay, overlayValue);
      return baseValue != overlayValue;
    }

    // Dotted paths of every key that exists in both tables (recursively) where
    // the overlay entry replaces the base entry with a different value. A
    // table-vs-scalar kind change replaces the whole subtree, so it counts as
    // one shadowed key at that path. Keys present only in `overlay` are
    // intentional overrides and are skipped.
    void collectShadowedKeys(
        const toml::table& base, const toml::table& overlay, std::string& path, std::vector<std::string>& out
    ) {
      for (const auto& [key, overlayNode] : overlay) {
        const toml::node* baseNode = base.get(key);
        if (baseNode == nullptr) {
          continue;
        }
        const std::size_t pathLength = path.size();
        if (!path.empty()) {
          path += '.';
        }
        path += key.str();
        if (const auto* overlayTable = overlayNode.as_table(); overlayTable != nullptr) {
          if (const auto* baseTable = baseNode->as_table(); baseTable != nullptr) {
            collectShadowedKeys(*baseTable, *overlayTable, path, out);
          } else {
            out.push_back(path);
          }
        } else if (baseNode->is_table() || leafValuesDiffer(*baseNode, overlayNode)) {
          out.push_back(path);
        }
        path.resize(pathLength);
      }
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
    std::set<std::filesystem::path> visited;
    for (const auto& root : roots) {
      if (anyOptOut && !root.optOut) {
        continue;
      }
      ConfigService::deepMerge(out.merged, expandFile(root.path, root.table, visited, out));
    }

    return out;
  }

  void
  collectShadowedPlacementOverrides(const toml::table& base, const toml::table& overlay, schema::Diagnostics& diag) {
    constexpr std::size_t kMaxWarnings = 24;
    constexpr std::string_view kMessage =
        "overridden by state-dir settings.toml (where the widget editor and placement remaps save state); "
        "the value in the config directory is ignored";

    std::vector<std::string> shadowed;
    for (std::string_view section : {"desktop_widgets", "lockscreen_widgets"}) {
      const auto* overlaySection = overlay.get(section);
      const auto* baseSection = base.get(section);
      if (overlaySection == nullptr || baseSection == nullptr) {
        continue;
      }
      const auto* overlaySectionTable = overlaySection->as_table();
      const auto* baseSectionTable = baseSection->as_table();
      if (overlaySectionTable == nullptr || baseSectionTable == nullptr) {
        continue;
      }
      std::string path{section};
      collectShadowedKeys(*baseSectionTable, *overlaySectionTable, path, shadowed);
    }
    const std::size_t reported = std::min(shadowed.size(), kMaxWarnings);
    for (std::size_t i = 0; i < reported; ++i) {
      diag.warn(std::move(shadowed[i]), std::string{kMessage}, "config.shadowed-override");
    }
    if (shadowed.size() > kMaxWarnings) {
      diag.warn(
          "desktop_widgets",
          std::format("+{} more keys overridden by state-dir settings.toml", shadowed.size() - kMaxWarnings),
          "config.shadowed-override"
      );
    }
  }

} // namespace noctalia::config
