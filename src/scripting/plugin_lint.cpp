#include "scripting/plugin_lint.h"

#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace scripting {
  namespace {

    bool isIdentChar(char c) noexcept {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    struct GetConfigRead {
      std::string key;
      int line = 0;
    };

    struct ObsoleteConfigAccessor {
      std::string name;
      int line = 0;
    };

    struct ScanResult {
      std::vector<GetConfigRead> reads;
      std::vector<ObsoleteConfigAccessor> obsoleteAccessors;
      int dynamicCount = 0; // getConfig calls whose key is not a static string literal
    };

    bool isObsoleteConfigReceiver(std::string_view receiver) noexcept {
      return receiver == "barWidget" || receiver == "desktopWidget" || receiver == "panel" || receiver == "launcher";
    }

    // Walk Luau source, skipping comments and string bodies, and collect every
    // `getConfig` call. A call with a string-literal argument yields its key; a call
    // with a computed argument bumps dynamicCount (so we can't claim a key unread).
    ScanResult scanGetConfigCalls(std::string_view src) {
      ScanResult out;
      int line = 1;
      const std::size_t n = src.size();
      std::size_t i = 0;

      auto readQuoted = [&](char quote, std::size_t pos, std::string& key) -> std::optional<std::size_t> {
        std::string value;
        for (std::size_t j = pos + 1; j < n; ++j) {
          const char c = src[j];
          if (c == '\\' && j + 1 < n) {
            value.push_back(src[j + 1]);
            ++j;
            continue;
          }
          if (c == quote) {
            key = std::move(value);
            return j + 1;
          }
          if (c == '\n') {
            break; // unterminated on this line; treat as not-a-literal
          }
          value.push_back(c);
        }
        return std::nullopt;
      };

      while (i < n) {
        const char c = src[i];

        if (c == '\n') {
          ++line;
          ++i;
          continue;
        }

        // Comments: -- line, or --[[ block ]]
        if (c == '-' && i + 1 < n && src[i + 1] == '-') {
          if (i + 3 < n && src[i + 2] == '[' && src[i + 3] == '[') {
            i += 4;
            while (i + 1 < n && !(src[i] == ']' && src[i + 1] == ']')) {
              if (src[i] == '\n') {
                ++line;
              }
              ++i;
            }
            i += 2;
          } else {
            while (i < n && src[i] != '\n') {
              ++i;
            }
          }
          continue;
        }

        // String literals: skip their bodies so a `getConfig` inside text never matches.
        if (c == '"' || c == '\'') {
          std::string discard;
          if (auto end = readQuoted(c, i, discard); end.has_value()) {
            i = *end;
          } else {
            ++i;
          }
          continue;
        }
        if (c == '[' && i + 1 < n && src[i + 1] == '[') {
          i += 2;
          while (i + 1 < n && !(src[i] == ']' && src[i + 1] == ']')) {
            if (src[i] == '\n') {
              ++line;
            }
            ++i;
          }
          i += 2;
          continue;
        }

        // Identifier — the only place a getConfig call can begin.
        if (isIdentChar(c) && (i == 0 || !isIdentChar(src[i - 1]))) {
          std::size_t j = i;
          while (j < n && isIdentChar(src[j])) {
            ++j;
          }
          const std::string_view ident = src.substr(i, j - i);
          if (ident != "getConfig") {
            i = j;
            continue;
          }

          // getConfig is universal and lives only under noctalia.*. Record the old
          // entry-specific aliases so the author gets a useful error instead of a
          // nil-global runtime failure.
          std::size_t receiverEnd = i;
          while (receiverEnd > 0 && (src[receiverEnd - 1] == ' ' || src[receiverEnd - 1] == '\t')) {
            --receiverEnd;
          }
          if (receiverEnd > 0 && src[receiverEnd - 1] == '.') {
            std::size_t receiverStart = receiverEnd - 1;
            while (receiverStart > 0 && (src[receiverStart - 1] == ' ' || src[receiverStart - 1] == '\t')) {
              --receiverStart;
            }
            const std::size_t receiverNameEnd = receiverStart;
            while (receiverStart > 0 && isIdentChar(src[receiverStart - 1])) {
              --receiverStart;
            }
            const std::string_view receiver = src.substr(receiverStart, receiverNameEnd - receiverStart);
            if (isObsoleteConfigReceiver(receiver)) {
              out.obsoleteAccessors.push_back({.name = std::string(receiver) + ".getConfig", .line = line});
            }
          }

          // Parse the argument: optional '(' then a string literal (or it's dynamic).
          const int callLine = line;
          std::size_t k = j;
          auto skipWs = [&]() {
            while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) {
              if (src[k] == '\n') {
                ++line;
              }
              ++k;
            }
          };
          skipWs();
          if (k < n && src[k] == '(') {
            ++k;
            skipWs();
          }
          if (k < n && (src[k] == '"' || src[k] == '\'')) {
            std::string key;
            if (auto end = readQuoted(src[k], k, key); end.has_value()) {
              out.reads.push_back({.key = std::move(key), .line = callLine});
              i = *end;
              continue;
            }
            ++out.dynamicCount;
          } else if (k + 1 < n && src[k] == '[' && src[k + 1] == '[') {
            std::string value;
            std::size_t m = k + 2;
            while (m + 1 < n && !(src[m] == ']' && src[m + 1] == ']')) {
              value.push_back(src[m]);
              ++m;
            }
            out.reads.push_back({.key = std::move(value), .line = callLine});
            i = m + 2;
            continue;
          } else {
            ++out.dynamicCount; // computed key — can't resolve statically
          }
          i = j;
          continue;
        }

        ++i;
      }

      return out;
    }

    std::size_t editDistance(std::string_view a, std::string_view b) {
      const std::size_t la = a.size();
      const std::size_t lb = b.size();
      std::vector<std::size_t> prev(lb + 1);
      std::vector<std::size_t> cur(lb + 1);
      for (std::size_t j = 0; j <= lb; ++j) {
        prev[j] = j;
      }
      for (std::size_t i = 1; i <= la; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= lb; ++j) {
          const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
          cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
      }
      return prev[lb];
    }

    // Closest declared key to `key`, if near enough to be a plausible typo.
    std::string suggestKey(std::string_view key, const std::vector<std::string>& declared) {
      std::string best;
      std::size_t bestDist = 0;
      for (const auto& candidate : declared) {
        const std::size_t d = editDistance(key, candidate);
        if (best.empty() || d < bestDist) {
          best = candidate;
          bestDist = d;
        }
      }
      const std::size_t threshold = std::max<std::size_t>(2, key.size() / 3);
      return (!best.empty() && bestDist <= threshold) ? best : std::string{};
    }

  } // namespace

  bool PluginLintReport::ok() const noexcept {
    return error.empty() && std::ranges::none_of(findings, [](const auto& f) { return f.isError(); });
  }

  PluginLintReport lintPluginDir(const std::filesystem::path& dir) {
    PluginLintReport report;
    report.manifestPath = dir / "plugin.toml";

    std::string parseError;
    auto manifest = parsePluginManifest(report.manifestPath, &parseError);
    if (!manifest.has_value()) {
      report.error = parseError.empty() ? "failed to parse plugin.toml" : parseError;
      return report;
    }
    report.pluginId = manifest->id;

    std::unordered_set<std::string> pluginKeys;
    for (const auto& field : manifest->settings) {
      pluginKeys.insert(field.key);
    }

    std::unordered_set<std::string> pluginKeysRead;
    bool anyDynamic = false;

    for (const auto& entry : manifest->entries) {
      // Keys valid to read from this entry: its own + plugin-level. Host-injected
      // panel shell keys count as declared (so reads are fine) but are excluded
      // from the unread check below since the host consumes them, not plugin code.
      std::vector<std::string> declaredForRead;
      std::vector<std::string> ownReadable; // entry keys eligible for the unread check
      for (const auto& field : entry.settings) {
        declaredForRead.push_back(field.key);
        if (!isPanelShellSettingKey(entry.id, field.key)) {
          ownReadable.push_back(field.key);
        }
      }
      for (const auto& k : pluginKeys) {
        declaredForRead.push_back(k);
      }
      std::unordered_set<std::string> declaredSet(declaredForRead.begin(), declaredForRead.end());

      const std::filesystem::path entryPath = dir / entry.entry;
      std::ifstream file(entryPath, std::ios::binary);
      if (!file) {
        report.findings.push_back(
            {.kind = PluginLintFinding::Kind::MissingEntryFile,
             .scope = entry.id,
             .file = entry.entry,
             .key = entry.entry}
        );
        continue;
      }
      std::ostringstream ss;
      ss << file.rdbuf();
      const std::string source = ss.str();

      const ScanResult scan = scanGetConfigCalls(source);
      anyDynamic = anyDynamic || scan.dynamicCount > 0;

      for (const auto& obsolete : scan.obsoleteAccessors) {
        report.findings.push_back(
            {.kind = PluginLintFinding::Kind::ObsoleteConfigAccessor,
             .scope = entry.id,
             .file = entry.entry,
             .key = obsolete.name,
             .line = obsolete.line}
        );
      }

      std::unordered_set<std::string> readInEntry;
      for (const auto& read : scan.reads) {
        readInEntry.insert(read.key);
        if (pluginKeys.contains(read.key)) {
          pluginKeysRead.insert(read.key);
        }
        if (!declaredSet.contains(read.key)) {
          report.findings.push_back(
              {.kind = PluginLintFinding::Kind::ReadUndeclared,
               .scope = entry.id,
               .file = entry.entry,
               .key = read.key,
               .line = read.line,
               .suggestion = suggestKey(read.key, declaredForRead)}
          );
        }
      }

      // Entry-level declared-but-unread. Skip if this entry has a dynamic getConfig
      // (the key could be read through a computed key we can't see).
      if (scan.dynamicCount == 0) {
        for (const auto& key : ownReadable) {
          if (!readInEntry.contains(key)) {
            report.findings.push_back(
                {.kind = PluginLintFinding::Kind::DeclaredUnread, .scope = entry.id, .file = entry.entry, .key = key}
            );
          }
        }
      }
    }

    // Plugin-level declared-but-unread: not read by ANY entry, and no entry reads
    // settings dynamically.
    if (!anyDynamic) {
      for (const auto& field : manifest->settings) {
        if (!pluginKeysRead.contains(field.key)) {
          report.findings.push_back(
              {.kind = PluginLintFinding::Kind::DeclaredUnread, .scope = {}, .file = {}, .key = field.key}
          );
        }
      }
    }

    return report;
  }

} // namespace scripting

namespace noctalia::plugins {
  namespace {

    constexpr const char* kHelpText =
        "Usage: noctalia plugins <command> [paths]\n"
        "\n"
        "Offline tools for plugin authors (no running shell required).\n"
        "To manage installed plugins on the running instance, use 'noctalia msg plugins'.\n"
        "\n"
        "Commands:\n"
        "  lint [path ...]\n"
        "      Cross-check each plugin's declared settings against its getConfig() calls.\n"
        "      Reports settings read but not declared in plugin.toml (a runtime loud miss),\n"
        "      obsolete entry-specific getConfig aliases, settings declared but never read,\n"
        "      and entries pointing at a missing file.\n"
        "      A path may be a plugin directory or a directory of plugins. Defaults to '.'.\n"
        "      Exits 1 if any error-level problem is found.\n";

    // Resolve a CLI path argument to the plugin directories it covers: the path
    // itself if it holds a plugin.toml, otherwise its immediate subdirectories that do.
    std::vector<std::filesystem::path> resolvePluginDirs(const std::filesystem::path& path, std::string& error) {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        error = std::format("path not found: {}", path.string());
        return {};
      }

      std::filesystem::path dir = path;
      if (std::filesystem::is_regular_file(path, ec) && path.filename() == "plugin.toml") {
        dir = path.parent_path();
      }

      if (std::filesystem::exists(dir / "plugin.toml", ec)) {
        return {dir};
      }

      std::vector<std::filesystem::path> dirs;
      if (std::filesystem::is_directory(dir, ec)) {
        for (const auto& sub : std::filesystem::directory_iterator(dir, ec)) {
          if (sub.is_directory(ec) && std::filesystem::exists(sub.path() / "plugin.toml", ec)) {
            dirs.push_back(sub.path());
          }
        }
      }
      std::ranges::sort(dirs);
      if (dirs.empty()) {
        error = std::format("no plugin.toml found in or under {}", path.string());
      }
      return dirs;
    }

    void printReport(const scripting::PluginLintReport& report, int& errors, int& warnings) {
      using Kind = scripting::PluginLintFinding::Kind;
      const std::string label = report.pluginId.empty() ? report.manifestPath.string() : report.pluginId;

      if (!report.error.empty()) {
        std::println("{}", label);
        std::println("  error  {}", report.error);
        ++errors;
        return;
      }

      std::println("{}", label);
      if (report.findings.empty()) {
        std::println("  ok");
        return;
      }

      for (const auto& f : report.findings) {
        const std::string where = f.line > 0 ? std::format("{}:{}", f.file, f.line)
            : !f.file.empty()                ? f.file
                                             : "plugin";
        switch (f.kind) {
        case Kind::ReadUndeclared: {
          const std::string hint =
              f.suggestion.empty() ? std::string{} : std::format(" (did you mean '{}'?)", f.suggestion);
          std::println("  error  {}  reads undeclared setting '{}'{}", where, f.key, hint);
          ++errors;
          break;
        }
        case Kind::ObsoleteConfigAccessor:
          std::println("  error  {}  '{}' was removed; use noctalia.getConfig", where, f.key);
          ++errors;
          break;
        case Kind::MissingEntryFile:
          std::println("  error  {}  entry '{}' points at a missing file '{}'", report.pluginId, f.scope, f.key);
          ++errors;
          break;
        case Kind::DeclaredUnread: {
          const std::string scope = f.scope.empty() ? std::string{"plugin-level"} : std::format("entry '{}'", f.scope);
          std::println("  warn   {}  declared setting '{}' ({}) is never read", where, f.key, scope);
          ++warnings;
          break;
        }
        }
      }
    }

    int runLint(int argc, char* argv[]) {
      std::vector<std::filesystem::path> args;
      for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
          std::println("{}", kHelpText);
          return 0;
        }
        args.emplace_back(argv[i]);
      }
      if (args.empty()) {
        args.emplace_back(".");
      }

      std::vector<std::filesystem::path> dirs;
      for (const auto& arg : args) {
        std::string error;
        const auto resolved = resolvePluginDirs(FileUtils::expandUserPath(arg.string()), error);
        if (!error.empty()) {
          std::println(stderr, "error: {}", error);
          return 1;
        }
        for (const auto& d : resolved) {
          if (std::ranges::find(dirs, d) == dirs.end()) {
            dirs.push_back(d);
          }
        }
      }

      int errors = 0;
      int warnings = 0;
      for (const auto& dir : dirs) {
        printReport(scripting::lintPluginDir(dir), errors, warnings);
      }

      std::println("");
      std::println(
          "{} {}, {} {}", errors, errors == 1 ? "error" : "errors", warnings, warnings == 1 ? "warning" : "warnings"
      );
      return errors > 0 ? 1 : 0;
    }

  } // namespace

  int runCli(int argc, char* argv[]) {
    // argv[0] = "noctalia", argv[1] = "plugins"; commands start at argv[2].
    if (argc < 3) {
      std::println(stderr, "{}", kHelpText);
      return 1;
    }
    const char* command = argv[2];
    if (std::strcmp(command, "--help") == 0 || std::strcmp(command, "-h") == 0) {
      std::println("{}", kHelpText);
      return 0;
    }
    if (std::strcmp(command, "lint") == 0) {
      return runLint(argc - 3, argv + 3);
    }
    std::println(stderr, "error: unknown plugins command '{}'\n", command);
    std::println(stderr, "{}", kHelpText);
    return 1;
  }

} // namespace noctalia::plugins
