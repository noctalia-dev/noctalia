#pragma once

#include <chrono>
#include <cstdint>
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
    std::chrono::system_clock::time_point last_modified;
    std::chrono::system_clock::time_point date_added;
    std::string author;
    std::string icon;
    std::string description;
    std::string license = "MIT";
    std::uint32_t pluginApiVersion = 0;
    bool deprecated = false;
    bool compatible = false;
  };

  struct CatalogResult {
    bool ok = false;
    std::string error;
    std::vector<CatalogEntry> entries;
  };

  // How discoverCatalog may obtain a git source's catalog. Path sources are always
  // read straight from disk, so the mode only affects git sources.
  enum class CatalogAccess : std::uint8_t {
    // Clone a missing repo and lazy-fetch catalog blobs. Network-bound: worker
    // threads only.
    Network,
    // Local git data only: never clone, never lazy-fetch. A missing repo or blob is a
    // failed result, not a network round-trip, so this is safe on the main thread.
    LocalOnly,
  };

  // Discover the plugins a source offers. git sources read `catalog.toml` from a repo
  // cache (blobless, no-checkout) via `git show`, obeying `access`; path sources read
  // it straight from disk, falling back to scanning `*/plugin.toml`. Compatibility is
  // computed against the supported plugin API range so the list can badge incompatible
  // plugins before any detail fetch.
  [[nodiscard]] CatalogResult discoverCatalog(const PluginSourceConfig& source, CatalogAccess access);

  // Parse a `catalog.toml` body. Exposed for testing + the git-source path.
  [[nodiscard]] std::vector<CatalogEntry> parseCatalogToml(const std::string& body);

} // namespace scripting
