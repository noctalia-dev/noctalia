#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace scripting {

  // A single lint problem found in a plugin.
  struct PluginLintFinding {
    enum class Kind {
      ReadUndeclared,         // getConfig("key") where "key" is not declared in plugin.toml (a runtime loud miss)
      ObsoleteConfigAccessor, // entry-specific getConfig alias removed in favor of noctalia.getConfig
      DeclaredUnread,         // a declared setting that no entry ever reads
      MissingEntryFile,       // an [[entry]] points at a .luau file that does not exist
    };

    Kind kind;
    std::string scope;      // entry id, or empty for a plugin-level setting
    std::string file;       // entry .luau filename, or empty
    std::string key;        // setting key, obsolete accessor, or missing filename
    int line = 0;           // 1-based source line for source findings, else 0
    std::string suggestion; // nearest declared key for ReadUndeclared, else empty

    // Every finding except DeclaredUnread is a real bug and fails the lint.
    [[nodiscard]] bool isError() const noexcept { return kind != Kind::DeclaredUnread; }
  };

  struct PluginLintReport {
    std::filesystem::path manifestPath;
    std::string pluginId;
    std::string error; // hard failure (unreadable/invalid manifest); findings then empty
    std::vector<PluginLintFinding> findings;

    [[nodiscard]] bool ok() const noexcept;
  };

  // Lint the plugin whose plugin.toml lives in `dir`: cross-check declared settings
  // against the `getConfig` calls in each entry's source. Pure (no live state).
  [[nodiscard]] PluginLintReport lintPluginDir(const std::filesystem::path& dir);

} // namespace scripting

namespace noctalia::plugins {

  // Entry point for `noctalia plugins <command> [paths]`. Returns a process exit
  // code. Offline author tool; does not start Application or talk to a running shell.
  int runCli(int argc, char* argv[]);

} // namespace noctalia::plugins
