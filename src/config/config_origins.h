#pragma once

#include "config/schema/diagnostics.h"
#include "core/toml.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace noctalia::config {

  // Dotted config path to the file and position that defined it.
  //
  // toml++ drops a node's source region whenever the node is copied, so the
  // merged config table carries no positions at all. Origins are captured per
  // file while it is still the freshly parsed table: `record` overwrites, so
  // record files in merge order and the last definition wins, matching
  // ConfigService::deepMerge.
  class ConfigOriginIndex {
  public:
    // Indexes every key in `tbl`, recursively. Nodes without a source region
    // (synthesized in memory rather than parsed) are skipped.
    void record(const std::filesystem::path& file, const toml::table& tbl);

    // Origin of `path`, or of its nearest recorded ancestor when the exact path
    // is not indexed (a value inside an array resolves to the array itself).
    // Null when no ancestor is known either.
    [[nodiscard]] const schema::SourceOrigin* find(std::string_view path) const;

    // Fills in the origin of every entry that does not already carry one.
    void annotate(schema::Diagnostics& diag) const;

  private:
    struct PathHash {
      using is_transparent = void;
      std::size_t operator()(std::string_view path) const noexcept { return std::hash<std::string_view>{}(path); }
    };

    // Indexes `node` under the dotted `path`, then recurses into its children.
    // `path` is reused as a scratch buffer and restored on return.
    void recordInto(std::string& path, const toml::node& node, const std::string& file);

    std::unordered_map<std::string, schema::SourceOrigin, PathHash, std::equal_to<>> m_entries;
  };

  // Position of a toml++ parse failure, attributed to `file`.
  [[nodiscard]] schema::SourceOrigin
  parseErrorOrigin(const toml::parse_error& error, const std::filesystem::path& file);

} // namespace noctalia::config
