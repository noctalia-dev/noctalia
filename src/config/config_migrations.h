#pragma once

#include "config/schema/diagnostics.h"
#include "core/toml.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::config {

  inline constexpr std::string_view kConfigVersionKey = "config_version";

  struct ConfigMigration {
    int toVersion;
    std::string_view summary;
    // `root` is the sidecar being migrated; `base` is the config.toml merge it overlays, read-only.
    void (*apply)(toml::table& root, const toml::table& base, schema::Diagnostics& diag);
  };

  struct LegacyConfigIssue {
    int migrationVersion;
    std::string path;
    std::string message;
    schema::SourceOrigin origin;

    bool operator==(const LegacyConfigIssue&) const = default;
  };

  using LegacyConfigIssues = std::vector<LegacyConfigIssue>;

  const std::vector<ConfigMigration>& configMigrations();
  int currentConfigVersion();
  std::optional<int> storedConfigVersion(const toml::table& root, schema::Diagnostics& diag);
  [[nodiscard]] int applyPendingConfigMigrations(
      toml::table& root, const toml::table& base, int storedVersion, schema::Diagnostics& diag,
      std::span<const ConfigMigration> migrations = configMigrations()
  );

  // `sidecar` is the settings.toml overlay already merged into `root`, if any. Its sidecar migrations
  // have run, so sunset/sunrise times it carries never imply the legacy custom schedule.
  void normalizeLegacyConfig(toml::table& root, LegacyConfigIssues& issues, const toml::table* sidecar = nullptr);

  std::string legacyConfigIssueFingerprint(const LegacyConfigIssues& issues);
  bool legacyConfigFingerprintHasNewIssues(std::string_view currentFingerprint, std::string_view previousFingerprint);
  bool legacyConfigReminderIntervalElapsed(std::int64_t nowEpochSeconds, std::int64_t previousEpochSeconds);

  inline constexpr std::int64_t kLegacyConfigReminderIntervalSeconds = 3 * 24 * 60 * 60;

} // namespace noctalia::config
