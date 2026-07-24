#include "config/config_migrations.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace noctalia::config {
  namespace {

    constexpr int kNegativeBarRadiusMigrationVersion = 1;
    constexpr int kCustomScheduleMigrationVersion = 2;
    constexpr int kShellMaterialMigrationVersion = 3;
    constexpr std::int64_t kMaxBarRadius = 500;
    constexpr std::array<std::string_view, 5> kBarRadiusKeys = {
        "radius", "radius_top_left", "radius_top_right", "radius_bottom_left", "radius_bottom_right",
    };

    bool migrateNegativeRadii(toml::table& table) {
      bool changed = false;
      for (const std::string_view key : kBarRadiusKeys) {
        const auto radius = table[key].value<std::int64_t>();
        if (!radius.has_value() || *radius >= 0) {
          continue;
        }

        const std::int64_t magnitude = *radius <= -kMaxBarRadius ? kMaxBarRadius : -*radius;
        table.insert_or_assign(key, magnitude);
        changed = true;
      }

      if (changed) {
        table.insert_or_assign("concave_edge_corners", true);
      }
      return changed;
    }

    template <typename OnChanged> void migrateNegativeBarRadii(toml::table& root, OnChanged&& onChanged) {
      auto* bars = root["bar"].as_table();
      if (bars == nullptr) {
        return;
      }

      for (auto& [barName, barNode] : *bars) {
        auto* bar = barNode.as_table();
        if (bar == nullptr) {
          continue;
        }

        const std::string barPath = "bar." + std::string(barName.str());
        if (migrateNegativeRadii(*bar)) {
          onChanged(barPath);
        }

        auto* monitors = (*bar)["monitor"].as_table();
        if (monitors == nullptr) {
          continue;
        }
        for (auto& [monitorName, monitorNode] : *monitors) {
          auto* monitor = monitorNode.as_table();
          if (monitor != nullptr && migrateNegativeRadii(*monitor)) {
            onChanged(barPath + ".monitor." + std::string(monitorName.str()));
          }
        }
      }
    }

    void migrateNegativeBarRadiiSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateNegativeBarRadii(root, [&diag](const std::string& path) {
        diag.warn(path, "migrated negative corner radii to concave_edge_corners");
      });
    }

    // sunset/sunrise used to schedule day/night on their own whenever no coordinates were available.
    // They now require custom_schedule. Turn it on for configs that relied on the old behavior,
    // i.e. times are set and no coordinate source is.
    template <typename OnChanged> void migrateCustomSchedule(toml::table& root, OnChanged&& onChanged) {
      auto* location = root["location"].as_table();
      if (location == nullptr || location->contains("custom_schedule")) {
        return;
      }

      const auto sunset = (*location)["sunset"].value<std::string>();
      const auto sunrise = (*location)["sunrise"].value<std::string>();
      if (!sunset.has_value() || sunset->empty() || !sunrise.has_value() || sunrise->empty()) {
        return;
      }

      const bool autoLocate = (*location)["auto_locate"].value_or(false);
      const bool hasAddress = !(*location)["address"].value_or(std::string_view{}).empty();
      const bool hasCoordinates = (*location)["latitude"].is_number() && (*location)["longitude"].is_number();
      if (autoLocate || hasAddress || hasCoordinates) {
        return;
      }

      location->insert_or_assign("custom_schedule", true);
      onChanged("location");
    }

    void migrateCustomScheduleSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateCustomSchedule(root, [&diag](const std::string& path) {
        diag.warn(path, "sunset/sunrise now require custom_schedule; enabled it to keep the schedule");
      });
    }

    // Lift legacy panel.transparency_mode / bar.glass into shell.material.
    void migrateShellMaterial(toml::table& root, schema::Diagnostics& diag) {
      auto* shell = root["shell"].as_table();
      if (shell == nullptr) {
        shell = root.insert("shell", toml::table{}).first->second.as_table();
      }
      if (shell == nullptr) {
        return;
      }

      auto* material = (*shell)["material"].as_table();
      const bool hadMaterialMode = material != nullptr && material->contains("mode");
      if (material == nullptr) {
        material = shell->insert("material", toml::table{}).first->second.as_table();
      }
      if (material == nullptr) {
        return;
      }

      if (!hadMaterialMode) {
        std::string mode = "solid";
        if (const auto* panel = (*shell)["panel"].as_table()) {
          if (const auto transparency = (*panel)["transparency_mode"].value<std::string>()) {
            const auto trimmed = *transparency;
            if (trimmed == "soft") {
              mode = "soft";
            } else if (trimmed == "glass" || trimmed == "liquid_glass") {
              mode = "liquid_glass";
            }
          }
        }
        // Any bar with glass = true upgrades the shell material.
        if (mode == "solid") {
          if (const auto* bars = root["bar"].as_table()) {
            for (const auto& [barName, barNode] : *bars) {
              (void)barName;
              if (const auto* bar = barNode.as_table()) {
                if ((*bar)["glass"].value_or(false)) {
                  mode = "liquid_glass";
                  break;
                }
                if (const auto* monitors = (*bar)["monitor"].as_table()) {
                  for (const auto& [monName, monNode] : *monitors) {
                    (void)monName;
                    if (const auto* mon = monNode.as_table(); mon != nullptr && (*mon)["glass"].value_or(false)) {
                      mode = "liquid_glass";
                      break;
                    }
                  }
                }
              }
              if (mode == "liquid_glass") {
                break;
              }
            }
          }
        }
        material->insert_or_assign("mode", mode);
        if (!material->contains("density")) {
          material->insert_or_assign("density", 1.0);
        }
        diag.warn("shell.material", "migrated surface material from panel transparency / bar.glass");
      }

      // Drop obsolete per-bar glass keys once the global material owns the look.
      if (auto* bars = root["bar"].as_table()) {
        for (auto& [barName, barNode] : *bars) {
          (void)barName;
          if (auto* bar = barNode.as_table()) {
            bar->erase("glass");
            if (auto* monitors = (*bar)["monitor"].as_table()) {
              for (auto& [monName, monNode] : *monitors) {
                (void)monName;
                if (auto* mon = monNode.as_table()) {
                  mon->erase("glass");
                }
              }
            }
          }
        }
      }
    }

    std::uint64_t stableIssueHash(int migrationVersion, std::string_view path) {
      constexpr std::uint64_t kOffset = 14695981039346656037ULL;
      constexpr std::uint64_t kPrime = 1099511628211ULL;
      std::uint64_t hash = kOffset;
      const auto append = [&hash](std::string_view value) {
        for (const unsigned char byte : value) {
          hash ^= byte;
          hash *= kPrime;
        }
      };
      append(std::to_string(migrationVersion));
      append(":");
      append(path);
      return hash;
    }

    std::set<std::string> splitFingerprint(std::string_view fingerprint) {
      std::set<std::string> entries;
      std::size_t start = 0;
      while (start < fingerprint.size()) {
        const std::size_t end = fingerprint.find(',', start);
        const std::size_t length = end == std::string_view::npos ? fingerprint.size() - start : end - start;
        if (length > 0) {
          entries.emplace(fingerprint.substr(start, length));
        }
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
      return entries;
    }

  } // namespace

  const std::vector<ConfigMigration>& configMigrations() {
    static const std::vector<ConfigMigration> migrations = {
        {
            .toVersion = kNegativeBarRadiusMigrationVersion,
            .summary = "bar: migrate negative corner radii",
            .apply = migrateNegativeBarRadiiSidecar,
        },
        {
            .toVersion = kCustomScheduleMigrationVersion,
            .summary = "location: opt legacy sunset/sunrise schedules into custom_schedule",
            .apply = migrateCustomScheduleSidecar,
        },
        {
            .toVersion = kShellMaterialMigrationVersion,
            .summary = "shell: lift panel transparency / bar.glass into shell.material",
            .apply = migrateShellMaterial,
        },
    };
    return migrations;
  }

  int currentConfigVersion() {
    const auto& migrations = configMigrations();
    return migrations.empty() ? 0 : migrations.back().toVersion;
  }

  std::optional<int> storedConfigVersion(const toml::table& root, schema::Diagnostics& diag) {
    const toml::node* node = root.get(kConfigVersionKey);
    if (node == nullptr) {
      return 0;
    }

    const auto value = node->value<std::int64_t>();
    if (!value.has_value()) {
      diag.fatal(std::string(kConfigVersionKey), "expected a non-negative integer", "config.version.invalid");
      return std::nullopt;
    }
    if (*value < 0 || *value > std::numeric_limits<int>::max()) {
      diag.fatal(std::string(kConfigVersionKey), "expected a non-negative integer", "config.version.invalid");
      return std::nullopt;
    }

    const int version = static_cast<int>(*value);
    if (version > currentConfigVersion()) {
      diag.fatal(
          std::string(kConfigVersionKey),
          std::format("version {} is newer than supported version {}", version, currentConfigVersion()),
          "config.version.unsupported"
      );
      return std::nullopt;
    }
    return version;
  }

  int applyPendingConfigMigrations(
      toml::table& root, int storedVersion, schema::Diagnostics& diag, std::span<const ConfigMigration> migrations
  ) {
    int appliedVersion = storedVersion;
    for (const ConfigMigration& migration : migrations) {
      if (migration.toVersion <= storedVersion) {
        continue;
      }
      migration.apply(root, diag);
      appliedVersion = migration.toVersion;
    }
    return appliedVersion;
  }

  void normalizeLegacyConfig(toml::table& root, LegacyConfigIssues& issues) {
    migrateNegativeBarRadii(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kNegativeBarRadiusMigrationVersion,
          .path = path,
          .message = "negative corner radii are deprecated; use positive radii and concave_edge_corners = true",
      });
    });
    migrateCustomSchedule(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kCustomScheduleMigrationVersion,
          .path = path,
          .message = "sunset/sunrise no longer schedule on their own; set custom_schedule = true",
      });
    });
  }

  std::string legacyConfigIssueFingerprint(const LegacyConfigIssues& issues) {
    std::set<std::string> entries;
    for (const LegacyConfigIssue& issue : issues) {
      entries.insert(std::format("{:016x}", stableIssueHash(issue.migrationVersion, issue.path)));
    }

    std::string fingerprint;
    for (const std::string& entry : entries) {
      if (!fingerprint.empty()) {
        fingerprint.push_back(',');
      }
      fingerprint += entry;
    }
    return fingerprint;
  }

  bool legacyConfigFingerprintHasNewIssues(std::string_view currentFingerprint, std::string_view previousFingerprint) {
    const std::set<std::string> current = splitFingerprint(currentFingerprint);
    const std::set<std::string> previous = splitFingerprint(previousFingerprint);
    return std::ranges::any_of(current, [&previous](const std::string& entry) { return !previous.contains(entry); });
  }

  bool legacyConfigReminderIntervalElapsed(std::int64_t nowEpochSeconds, std::int64_t previousEpochSeconds) {
    return previousEpochSeconds > nowEpochSeconds
        || nowEpochSeconds - previousEpochSeconds >= kLegacyConfigReminderIntervalSeconds;
  }

} // namespace noctalia::config
