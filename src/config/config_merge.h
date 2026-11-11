#pragma once

#include "config/config_origins.h"
#include "config/schema/diagnostics.h"
#include "config/toml.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::config {

  // Result of merging all config-dir files, including any pulled in via [include].
  struct [[nodiscard]] MergeResult {
    // The merged TOML with every file's [include] table stripped out.
    toml::table merged;
    // Every file actually loaded (root + included), canonicalized, in load order.
    std::vector<std::filesystem::path> loadedFiles;
    // Directories named directly in an [include].files list, plus every existing
    // system config dir (canonicalized). Used to watch for newly-created *.toml within them.
    std::vector<std::filesystem::path> watchedDirs;
    // First parse / missing-include error encountered, empty if none. The text
    // carries no location; firstErrorOrigin has it.
    std::string firstError;
    schema::SourceOrigin firstErrorOrigin;
    // Source position of every merged key. `merged` cannot carry it: toml++
    // drops source regions on copy.
    ConfigOriginIndex origins;
  };

  // Merges every system config dir (least important first), then `configDir` on top.
  // Each dir is scanned for *.toml (alphabetical) and merged, honoring each
  // file's optional [include] table:
  //   - [include].files lists extra files and directories to pull in. A file's
  //     includes are merged first (a reusable base); the file's own body then
  //     overlays on top (host wins). Relative paths resolve against the including
  //     file's own directory; ~, $VAR and ${VAR} are expanded.
  //   - [include].autoload = false in a root file restricts that dir's load set to
  //     only the root file(s) that set it (plus their recursive includes).
  // Each file is loaded at most once; cycles are warned and skipped. The caller is
  // responsible for overlaying settings.toml afterwards (it is never include-expanded).
  std::vector<std::string> systemConfigDirs();

  MergeResult mergeConfigWithIncludes(std::string_view configDir, std::span<const std::string> systemConfigDirs);

} // namespace noctalia::config
