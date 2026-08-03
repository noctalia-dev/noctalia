#include "scripting/plugin_lint.h"

#include "cli/parse.h"
#include "cli/schema_plugins.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
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
      std::vector<std::string> requirePaths;
      int dynamicCount = 0; // getConfig calls whose key is not a static string literal
    };

    bool isObsoleteConfigReceiver(std::string_view receiver) noexcept {
      return receiver == "barWidget" || receiver == "desktopWidget" || receiver == "panel" || receiver == "launcher";
    }

    // Walk Luau source, skipping comments and string bodies, and collect static
    // `getConfig` reads, obsolete entry-specific accessors, and static `require`
    // imports. A getConfig call with a computed argument bumps dynamicCount.
    ScanResult scanLuauSource(std::string_view src) {
      ScanResult out;
      int line = 1;
      const std::size_t n = src.size();
      std::size_t i = 0;

      auto readQuoted = [&](char quote, std::size_t pos, std::string& value) -> std::optional<std::size_t> {
        std::string parsed;
        for (std::size_t j = pos + 1; j < n; ++j) {
          const char c = src[j];
          if (c == '\\' && j + 1 < n) {
            parsed.push_back(src[j + 1]);
            ++j;
            continue;
          }
          if (c == quote) {
            value = std::move(parsed);
            return j + 1;
          }
          if (c == '\n') {
            break; // unterminated on this line; treat as not-a-literal
          }
          parsed.push_back(c);
        }
        return std::nullopt;
      };

      auto readBlockString = [&](std::size_t pos, std::string& value, int& newlineCount) -> std::optional<std::size_t> {
        std::string parsed;
        newlineCount = 0;
        std::size_t j = pos + 2;
        while (j + 1 < n && !(src[j] == ']' && src[j + 1] == ']')) {
          if (src[j] == '\n') {
            ++newlineCount;
          }
          parsed.push_back(src[j]);
          ++j;
        }
        if (j + 1 >= n) {
          return std::nullopt;
        }
        value = std::move(parsed);
        return j + 2;
      };

      auto parseStaticStringArgument = [&](std::size_t pos, std::string& value,
                                           int& parsedLine) -> std::optional<std::size_t> {
        auto skipWsAt = [&](std::size_t& k, int& lineAtK) {
          while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) {
            if (src[k] == '\n') {
              ++lineAtK;
            }
            ++k;
          }
        };

        std::size_t k = pos;
        skipWsAt(k, parsedLine);
        const bool parenthesized = k < n && src[k] == '(';
        if (parenthesized) {
          ++k;
          skipWsAt(k, parsedLine);
        }

        std::optional<std::size_t> literalEnd;
        int literalNewlines = 0;
        if (k < n && (src[k] == '"' || src[k] == '\'')) {
          literalEnd = readQuoted(src[k], k, value);
        } else if (k + 1 < n && src[k] == '[' && src[k + 1] == '[') {
          literalEnd = readBlockString(k, value, literalNewlines);
        }
        if (!literalEnd.has_value()) {
          return std::nullopt;
        }

        int endLine = parsedLine + literalNewlines;
        std::size_t end = *literalEnd;
        if (!parenthesized) {
          parsedLine = endLine;
          return end;
        }

        skipWsAt(end, endLine);
        if (end >= n || src[end] != ')') {
          return std::nullopt;
        }
        parsedLine = endLine;
        return end + 1;
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
            i = (i + 1 < n) ? i + 2 : n;
          } else {
            while (i < n && src[i] != '\n') {
              ++i;
            }
          }
          continue;
        }

        // String literals: skip their bodies so source-looking text never matches.
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
          int blockNewlines = 0;
          std::string discard;
          if (auto end = readBlockString(i, discard, blockNewlines); end.has_value()) {
            line += blockNewlines;
            i = *end;
          } else {
            i = n;
          }
          continue;
        }

        // Identifier — the only place a call name can begin.
        if (isIdentChar(c) && (i == 0 || !isIdentChar(src[i - 1]))) {
          std::size_t j = i;
          while (j < n && isIdentChar(src[j])) {
            ++j;
          }
          const std::string_view ident = src.substr(i, j - i);
          if (ident != "getConfig" && ident != "require") {
            i = j;
            continue;
          }

          if (ident == "getConfig") {
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
          } else {
            std::size_t receiverEnd = i;
            while (receiverEnd > 0 && (src[receiverEnd - 1] == ' ' || src[receiverEnd - 1] == '\t')) {
              --receiverEnd;
            }
            if (receiverEnd > 0 && (src[receiverEnd - 1] == '.' || src[receiverEnd - 1] == ':')) {
              i = j;
              continue;
            }
          }

          const int callLine = line;
          int parsedLine = line;
          std::string value;
          if (auto end = parseStaticStringArgument(j, value, parsedLine); end.has_value()) {
            if (ident == "getConfig") {
              out.reads.push_back({.key = std::move(value), .line = callLine});
            } else {
              out.requirePaths.push_back(std::move(value));
            }
            line = parsedLine;
            i = *end;
            continue;
          }

          if (ident == "getConfig") {
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

    bool isFollowableRequirePath(std::string_view path) noexcept {
      return (path.starts_with("./") || path.starts_with("../")) && path.ends_with(".luau");
    }

    std::optional<std::filesystem::path> canonicalRegularFile(const std::filesystem::path& path) {
      std::error_code ec;
      std::filesystem::path canonical = std::filesystem::canonical(path, ec);
      if (ec) {
        return std::nullopt;
      }
      const bool regular = std::filesystem::is_regular_file(canonical, ec);
      if (ec || !regular) {
        return std::nullopt;
      }
      return canonical;
    }

    std::filesystem::path normalizedPluginRoot(const std::filesystem::path& dir) {
      std::error_code ec;
      std::filesystem::path root = std::filesystem::weakly_canonical(dir, ec);
      if (!ec) {
        return root;
      }
      root = std::filesystem::absolute(dir, ec);
      return ec ? dir.lexically_normal() : root.lexically_normal();
    }

    std::string sourceFileForFinding(
        const std::filesystem::path& pluginRoot, const std::filesystem::path& sourcePath, std::string_view fallback
    ) {
      std::error_code ec;
      const std::filesystem::path relative = std::filesystem::relative(sourcePath, pluginRoot, ec);
      if (!ec && !relative.empty()) {
        return relative.generic_string();
      }
      return fallback.empty() ? sourcePath.lexically_normal().generic_string() : std::string(fallback);
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

    const std::filesystem::path pluginRoot = normalizedPluginRoot(dir);
    std::unordered_set<std::string> pluginKeysRead;
    bool anyDynamic = false;

    struct SourceToScan {
      std::filesystem::path path;
      std::string file;
    };

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
      std::optional<std::filesystem::path> entryCanonical = canonicalRegularFile(entryPath);
      if (!entryCanonical.has_value()) {
        report.findings.push_back(
            {.kind = PluginLintFinding::Kind::MissingEntryFile,
             .scope = entry.id,
             .file = entry.entry,
             .key = entry.entry}
        );
        continue;
      }

      std::vector<SourceToScan> pending;
      pending.push_back({.path = *entryCanonical, .file = entry.entry});
      std::unordered_set<std::string> visited;
      visited.insert(entryCanonical->generic_string());
      std::unordered_set<std::string> readInEntry;
      int entryDynamicCount = 0;

      while (!pending.empty()) {
        SourceToScan sourceToScan = std::move(pending.back());
        pending.pop_back();

        std::ifstream file(sourceToScan.path, std::ios::binary);
        if (!file) {
          continue;
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        const std::string source = ss.str();

        const ScanResult scan = scanLuauSource(source);
        entryDynamicCount += scan.dynamicCount;

        for (const auto& obsolete : scan.obsoleteAccessors) {
          report.findings.push_back(
              {.kind = PluginLintFinding::Kind::ObsoleteConfigAccessor,
               .scope = entry.id,
               .file = sourceToScan.file,
               .key = obsolete.name,
               .line = obsolete.line}
          );
        }

        for (const auto& read : scan.reads) {
          readInEntry.insert(read.key);
          if (pluginKeys.contains(read.key)) {
            pluginKeysRead.insert(read.key);
          }
          if (!declaredSet.contains(read.key)) {
            report.findings.push_back(
                {.kind = PluginLintFinding::Kind::ReadUndeclared,
                 .scope = entry.id,
                 .file = sourceToScan.file,
                 .key = read.key,
                 .line = read.line,
                 .suggestion = suggestKey(read.key, declaredForRead)}
            );
          }
        }

        for (const auto& requirePath : scan.requirePaths) {
          if (!isFollowableRequirePath(requirePath)) {
            continue;
          }
          std::optional<std::filesystem::path> moduleCanonical =
              canonicalRegularFile(sourceToScan.path.parent_path() / requirePath);
          if (!moduleCanonical.has_value()) {
            continue;
          }
          const std::string moduleKey = moduleCanonical->generic_string();
          if (!visited.insert(moduleKey).second) {
            continue;
          }
          pending.push_back(
              {.path = *moduleCanonical, .file = sourceFileForFinding(pluginRoot, *moduleCanonical, requirePath)}
          );
        }
      }

      anyDynamic = anyDynamic || entryDynamicCount > 0;

      // Entry-level declared-but-unread. Skip if this entry has a dynamic getConfig
      // (the key could be read through a computed key we can't see).
      if (entryDynamicCount == 0) {
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
        "      Cross-check each plugin's declared settings against getConfig() calls in entries and static modules.\n"
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
    if (argc < 3) {
      std::println(stderr, "{}", kHelpText);
      return 1;
    }

    const std::string_view subcmd = argv[2];
    if (subcmd == "--help" || subcmd == "-h") {
      std::println("{}", kHelpText);
      return 0;
    }

    if (subcmd == "lint") {
      const auto parsed = cli_schema::parseArgs(argc, argv, 3, kLintCmd);
      if (!parsed) {
        std::println(stderr, "error: {}", parsed.error());
        std::println(stderr, "Run 'noctalia plugins lint --help' for usage.");
        return 1;
      }
      if (parsed->helpRequested) {
        std::println("{}", kHelpText);
        return 0;
      }
      return runLint(argc - 3, argv + 3);
    }

    std::println(stderr, "error: unknown plugins command '{}'\n", subcmd);
    std::println(stderr, "{}", kHelpText);
    return 1;
  }

} // namespace noctalia::plugins
