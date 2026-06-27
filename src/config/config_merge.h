#pragma once

#include "core/toml.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::config {

  // Result of merging all config-dir files, including any pulled in via [include].
  struct MergeResult {
    // The merged TOML with every file's [include] table stripped out.
    toml::table merged;
    // Every file actually loaded (root + included), canonicalized, in load order.
    std::vector<std::filesystem::path> loadedFiles;
    // Directories named directly in an [include].files list (canonicalized).
    // Used to watch for newly-created *.toml within them.
    std::vector<std::filesystem::path> includeDirs;
    // First parse / missing-include error encountered, empty if none.
    std::string firstError;
  };

  // Scans `configDir` for *.toml (alphabetical) and merges them, honoring each
  // file's optional [include] table:
  //   - [include].files lists extra files and directories to pull in. A file's
  //     includes are merged first (a reusable base); the file's own body then
  //     overlays on top (host wins). Relative paths resolve against the including
  //     file's own directory; ~, $VAR and ${VAR} are expanded.
  //   - [include].autoload = false in a root file restricts the load set to only
  //     the root file(s) that set it (plus their recursive includes).
  // Each file is loaded at most once; cycles are warned and skipped. The caller is
  // responsible for overlaying settings.toml afterwards (it is never include-expanded).
  [[nodiscard]] MergeResult mergeConfigWithIncludes(std::string_view configDir);

} // namespace noctalia::config
