#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PluginSourceConfig;

namespace scripting {

  // An older revision of a plugin, published so a host below the tip's API level still
  // has something to install. Git sources only: a revision means nothing on disk.
  struct CatalogRelease {
    std::uint32_t pluginApiVersion = 0;
    std::string version;
    std::string revision; // full 40-char commit sha
  };

  // One row of a source's catalog: the minimum needed to render, search, and
  // compat-check a browsable list — never the full plugin.toml.
  struct CatalogEntry {
    std::string id;   // "author/plugin"
    std::string name; // mandatory display name
    std::vector<std::string> tags;
    std::vector<std::string> dependencies;
    std::string version;                             // latest available in the source
    std::chrono::system_clock::time_point updatedAt; // catalog key `updated_at`, epoch when absent
    std::chrono::system_clock::time_point addedAt;   // catalog key `added_at`, epoch when absent
    std::string author;
    std::string icon;
    std::string description;
    std::string license = "MIT";
    std::uint32_t pluginApiVersion = 0;
    bool deprecated = false;

    // Older revisions, sorted by descending pluginApiVersion. Each is the newest
    // revision at or below its level, so the highest supported one is also the newest.
    std::vector<CatalogRelease> releases;

    // The revision/version/API level this host will actually install, picked from the
    // tip or from `releases`. `compatible` means "something here is installable";
    // `heldBack` means the tip was too new and an older release was picked instead.
    std::string resolvedRevision;
    std::string resolvedVersion;
    std::uint32_t resolvedPluginApiVersion = 0;
    bool compatible = false;
    bool heldBack = false;
  };

  struct CatalogResult {
    bool ok = false;
    std::string error;
    std::vector<CatalogEntry> entries;
    // Exact commit used for a git catalog. Empty for path sources and failures.
    std::string revision;
  };

  [[nodiscard]] inline const CatalogEntry*
  findCatalogEntry(const std::vector<CatalogEntry>& entries, std::string_view id) {
    for (const auto& entry : entries) {
      if (entry.id == id) {
        return &entry;
      }
    }
    return nullptr;
  }

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

  // Parse a `catalog.toml` body and resolve every row against the supported plugin API
  // range. `headRevision` is the commit the body was read from; pass an empty string for
  // a path source, which disables `[[plugin.release]]` rows since it cannot export a
  // revision. Exposed for testing + the git-source path.
  [[nodiscard]] std::vector<CatalogEntry> parseCatalogToml(const std::string& body, std::string_view headRevision);

} // namespace scripting
