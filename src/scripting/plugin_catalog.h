#pragma once

#include <string>
#include <vector>

struct PluginSourceConfig;

namespace scripting {

  // One row of a source's catalog: the minimum needed to render, search, and
  // compat-check a browsable list — never the full plugin.toml.
  struct CatalogEntry {
    std::string id;   // "author/plugin"
    std::string name; // mandatory display name
    std::vector<std::string> tags;
    std::vector<std::string> dependencies;
    std::string version; // latest available in the source
    std::string author;
    std::string icon;
    std::string description;
    std::string license = "MIT";
    std::string minNoctalia;
    bool deprecated = false;
    bool compatible = true; // version::atLeast(appVersion, minNoctalia)
  };

  struct CatalogResult {
    bool ok = false;
    std::string error;
    std::vector<CatalogEntry> entries;
  };

  // Discover the plugins a source offers. git sources clone-if-needed into a repo
  // cache (blobless, no-checkout) and read `catalog.toml` via `git show`; path
  // sources read it straight from disk, falling back to scanning `*/plugin.toml`.
  // Blocking git/IO — call off the UI thread. Compatibility is computed against
  // the running version so the list can badge incompatible plugins before any
  // detail fetch.
  [[nodiscard]] CatalogResult discoverCatalog(const PluginSourceConfig& source);

  // Parse a `catalog.toml` body. Exposed for testing + the git-source path.
  [[nodiscard]] std::vector<CatalogEntry> parseCatalogToml(const std::string& body);

} // namespace scripting
