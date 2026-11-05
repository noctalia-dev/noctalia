#pragma once

#include <string>
#include <string_view>

// Names the application that embeds the library. The consumer sets it once at startup, before using
// logging, asset lookup, XDG directory helpers, thumbnails, or debug environment flags.
struct ApplicationIdentity {
  // Directory and file stem: XDG config/state/data/cache subdirectories, the installed asset
  // directory (<datadir>/<name>/assets), and the log file (<name>.log).
  std::string name;
  // Prefix of the environment variables the library reads, for example <envPrefix>_LOG_LEVEL,
  // <envPrefix>_ASSETS_DIR, <envPrefix>_CONFIG_HOME, and <envPrefix>_PROFILE.
  std::string envPrefix;
};

void setApplicationIdentity(ApplicationIdentity identity);

// Aborts if setApplicationIdentity() has not been called.
[[nodiscard]] const ApplicationIdentity& applicationIdentity();

// <envPrefix>_<suffix>
[[nodiscard]] std::string applicationEnvVar(std::string_view suffix);
