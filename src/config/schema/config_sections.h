#pragma once

#include "config/config_types.h"
#include "config/schema/diagnostics.h"
#include "core/toml.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// The top-level config section list. The loader (ConfigService::parseTable), the
// exporter (config_export::serialize), the validator (config_validate), schema path
// resolution and the round-trip test all iterate this table instead of restating the
// sections, so a section cannot exist in one of them and be missing from another.
namespace noctalia::config::schema {

  // One row per schema-backed section. Every closure is derived from the same
  // member pointer + section schema, so a row cannot be half-wired (read but not
  // written, validated but not known).
  struct SectionSpec {
    std::string_view name;

    // Reads the section table into its Config member. Throws on a hard value error,
    // leaving the member untouched.
    std::function<void(const toml::table& tbl, Config& out, Diagnostics& diag)> read;

    std::function<toml::table(const Config& in)> write;

    std::function<void(const toml::table& tbl, std::vector<std::string>& unknown)> collectUnknown;

    // Reads into a default-constructed section struct: surfaces enum/range diagnostics
    // with no Config to mutate. Throws like read().
    std::function<void(const toml::table& tbl, Diagnostics& diag)> checkAgainstDefaults;

    std::function<bool(const Config& a, const Config& b)> sectionEqual;

    // Keys under this section that are app-managed state rather than user settings,
    // so an "unknown setting" warning would be wrong (wallpaper's default/last/...).
    std::unordered_set<std::string> allowUnknownPaths;
  };

  [[nodiscard]] std::span<const SectionSpec> sections();

  [[nodiscard]] const SectionSpec* findSection(std::string_view name);

  // Root keys whose shape is not a plain section schema: named tables (bar, widget),
  // placement structures (desktop_widgets, lockscreen_widgets), the open per-plugin
  // map (plugin_settings), and config meta keys. Their read/write/validate stay
  // hand-written in the owning file; the names live here so the known-root-key set
  // still has exactly one source.
  [[nodiscard]] std::span<const std::string_view> customRootKeys();

  [[nodiscard]] bool isKnownRootKey(std::string_view name);

} // namespace noctalia::config::schema
