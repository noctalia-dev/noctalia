#include "config/config_service.h"

#include "compositors/compositor_detect.h"
#include "config/config_credentials.h"
#include "config/config_export.h"
#include "config/config_merge.h"
#include "config/config_migrations.h"
#include "config/config_origins.h"
#include "config/widget_config.h"
#include "config/schema/config_schema.h"
#include "config/schema/diagnostics.h"
#include "core/build_info.h"
#include "core/log.h"
#include "system/distro_info.h"
#include "system/hardware_info.h"
#include "util/string_utils.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <format>
#include "core/toml.h"

#include <algorithm>
#include <ctime>
#include <exception>
#include <iomanip>
#include <string>

namespace {
  constexpr Logger kLog("config");
  namespace schema = nythera::config::schema;
  std::string formatToml(const toml::table& table) { std::ostringstream out; out << toml::toml_formatter{table, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings}; return out.str(); }
  void seedLegacyDockAllowListEnabled(toml::table& sidecar, const toml::table& baseConfig) {
    auto* dock = sidecar["dock"].as_table();
    if (dock == nullptr || (*dock)["monitors"].as_array() == nullptr || dock->contains("enabled")) return;
    if (baseConfig["dock"]["enabled"].value_or(false)) dock->insert("enabled", true);
  }

  struct MergedUserConfigSources {
    toml::table merged;
    toml::table sidecar; // migrated settings.toml overlay already merged into `merged`
  };

  std::optional<MergedUserConfigSources>
  mergeUserConfigSources(std::string_view configDir, std::string_view settingsPath, std::string* error) {
    auto mergeResult = nythera::config::mergeConfigWithIncludes(configDir);
    toml::table merged = std::move(mergeResult.merged);
    if (!mergeResult.firstError.empty()) {
      const std::string located = mergeResult.firstErrorOrigin.prefixed(mergeResult.firstError);
      if (error != nullptr) {
        *error = located;
        return std::nullopt;
      }
      kLog.warn("skipping config error in merged user config export: {}", located);
    }

    toml::table sidecar;
    if (!settingsPath.empty() && std::filesystem::exists(std::filesystem::path(std::string(settingsPath)))) {
      const std::filesystem::path settingsFile{std::string(settingsPath)};
      try {
        sidecar = toml::parse_file(settingsPath);
        seedLegacyDockAllowListEnabled(sidecar, merged);
        nythera::config::ConfigOriginIndex origins;
        origins.record(settingsFile, sidecar);
        schema::Diagnostics migrationDiag;
        const auto storedVersion = nythera::config::storedConfigVersion(sidecar, migrationDiag);
        if (storedVersion.has_value()) {
          (void)nythera::config::applyPendingConfigMigrations(sidecar, merged, *storedVersion, migrationDiag);
        }
        origins.annotate(migrationDiag);
        for (const auto& entry : migrationDiag.entries) {
          if (entry.severity == schema::Diagnostics::Severity::Error) {
            if (error != nullptr) {
              *error = entry.describe();
            }
            return std::nullopt;
          }
          kLog.warn("{}", entry.describe());
        }
        ConfigService::deepMerge(merged, sidecar);
      } catch (const toml::parse_error& e) {
        if (error != nullptr) {
          *error = nythera::config::parseErrorOrigin(e, settingsFile).prefixed(e.description());
          return std::nullopt;
        }
        sidecar.clear();
        kLog.warn("skipping parse error in merged user config export {}: {}", settingsPath, e.description());
      }
    }

    if (error != nullptr) {
      error->clear();
    }
    return MergedUserConfigSources{.merged = std::move(merged), .sidecar = std::move(sidecar)};
  }


  std::vector<std::filesystem::path> sortedConfigTomlFiles(std::string_view configDir) {
    std::vector<std::filesystem::path> files;
    if (configDir.empty()) {
      return files;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(configDir, ec) || ec) {
      return files;
    }
    for (const auto& entry : std::filesystem::directory_iterator(configDir, ec)) {
      if (entry.is_regular_file() && entry.path().extension() == ".toml") {
        files.push_back(entry.path());
      }
    }
    std::ranges::sort(files);
    return files;
  }

  std::string readTextFile(const std::filesystem::path& path, std::string* error = nullptr) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
      if (error != nullptr) {
        *error = "open failed";
      }
      return {};
    }

    std::ostringstream out;
    out << in.rdbuf();
    if (!in.good() && !in.eof()) {
      if (error != nullptr) {
        *error = "read failed";
      }
      return {};
    }
    if (error != nullptr) {
      error->clear();
    }
    return out.str();
  }

  std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
  }

  std::string relativeTo(const std::filesystem::path& path, const std::filesystem::path& base) {
    const auto relative = path.lexically_relative(base);
    if (!relative.empty()) {
      return relative.string();
    }
    return path.filename().string();
  }

  void insertNonEmpty(toml::table& table, std::string_view key, const std::string& value) {
    if (!value.empty()) {
      table.insert_or_assign(std::string(key), value);
    }
  }

  toml::table buildDistroReport() {
    toml::table distro;
    distro.insert_or_assign("label", distroLabel());
    if (const auto detected = DistroDetector::detect(); detected.has_value()) {
      insertNonEmpty(distro, "id", detected->id);
      insertNonEmpty(distro, "name", detected->name);
      insertNonEmpty(distro, "version", detected->version);
      insertNonEmpty(distro, "pretty_name", detected->prettyName);
    }
    return distro;
  }

  toml::table buildCompositorReport() {
    const compositors::CompositorKind kind = compositors::detect();

    toml::table compositor;
    compositor.insert_or_assign("label", compositorLabel());
    compositor.insert_or_assign("name", std::string(compositors::name(kind)));
    const std::string_view hint = compositors::envHint();
    if (!hint.empty()) {
      compositor.insert_or_assign("env_hint", std::string(hint));
    }
    return compositor;
  }

}

std::string ConfigService::buildMergedUserConfig(nythera::config::CredentialExportMode credentials) const {
  return buildMergedUserConfigFromSources(m_configDir, m_overridesPath, nullptr, credentials);
}

std::string ConfigService::buildEffectiveConfig(nythera::config::CredentialExportMode credentials) const {
  if (credentials == nythera::config::CredentialExportMode::Keep) {
    return formatToml(*m_configTable) + "\n";
  }
  toml::table effective = *m_configTable;
  nythera::config::applyCredentialExportMode(effective, credentials);
  return formatToml(effective) + "\n";
}

std::string ConfigService::buildMergedUserConfigFromSources(
    std::string_view configDir, std::string_view settingsPath, std::string* error,
    nythera::config::CredentialExportMode credentials
) {
  const auto sources = mergeUserConfigSources(configDir, settingsPath, error);
  if (!sources.has_value()) {
    return {};
  }
  toml::table normalized = sources->merged;
  normalized.erase(nythera::config::kConfigVersionKey);
  nythera::config::LegacyConfigIssues issues;
  nythera::config::normalizeLegacyConfig(normalized, issues, &sources->sidecar);
  nythera::config::applyCredentialExportMode(normalized, credentials);
  return formatToml(normalized) + "\n";
}

std::string ConfigService::buildEffectiveConfigFromSources(
    std::string_view configDir, std::string_view settingsPath, std::string* error,
    nythera::config::CredentialExportMode credentials
) {
  const auto sources = mergeUserConfigSources(configDir, settingsPath, error);
  if (!sources.has_value()) {
    return {};
  }

  toml::table normalized = sources->merged;
  normalized.erase(nythera::config::kConfigVersionKey);
  nythera::config::LegacyConfigIssues issues;
  nythera::config::normalizeLegacyConfig(normalized, issues, &sources->sidecar);

  Config config;
  nythera::config::seedBuiltinWidgets(config);
  if (normalized.empty()) {
    config = makeDefaultConfig();
  } else {
    try {
      parseConfigTable(normalized, config, false, false);
    } catch (const std::exception& e) {
      if (error != nullptr) {
        *error = e.what();
      }
      return {};
    }
  }

  if (error != nullptr) {
    error->clear();
  }
  toml::table effective = config_export::serialize(config);
  nythera::config::applyCredentialExportMode(effective, credentials);
  return formatToml(effective) + "\n";
}

std::string ConfigService::buildSupportReport() const {
  toml::table root;

  toml::table report;
  report.insert_or_assign("format_version", std::int64_t{1});
  report.insert_or_assign("generated_by", "nythera");
  report.insert_or_assign("generated_at_utc", utcTimestamp());
  report.insert_or_assign("nythera_version", std::string(nythera::build_info::version()));
  report.insert_or_assign("git_revision", std::string(nythera::build_info::revision()));
  root.insert_or_assign("report", std::move(report));

  toml::table system;
  system.insert_or_assign("distro", buildDistroReport());
  system.insert_or_assign("compositor", buildCompositorReport());
  root.insert_or_assign("system", std::move(system));

  toml::table paths;
  paths.insert_or_assign("config_dir", m_configDir);
  paths.insert_or_assign("settings_path", m_overridesPath);
  paths.insert_or_assign("state_path", m_stateStore.path().string());
  root.insert_or_assign("paths", std::move(paths));

  toml::table merged;
  toml::array sources;
  const auto configFiles = sortedConfigTomlFiles(m_configDir);
  for (std::size_t i = 0; i < configFiles.size(); ++i) {
    const auto& path = configFiles[i];

    toml::table source;
    source.insert_or_assign("kind", "declarative");
    source.insert_or_assign("load_order", static_cast<std::int64_t>(i));
    source.insert_or_assign("relative_path", relativeTo(path, m_configDir));
    source.insert_or_assign("path", path.string());

    std::string readError;
    source.insert_or_assign("content", readTextFile(path, &readError));
    if (!readError.empty()) {
      source.insert_or_assign("read_error", readError);
    } else {
      try {
        auto table = toml::parse_file(path.string());
        deepMerge(merged, table);
      } catch (const toml::parse_error& e) {
        source.insert_or_assign("parse_error", e.what());
      }
    }

    sources.push_back(std::move(source));
  }
  root.insert_or_assign("config_sources", std::move(sources));

  toml::table state;
  state.insert_or_assign("kind", "state");
  state.insert_or_assign("relative_path", "settings.toml");
  state.insert_or_assign("path", m_overridesPath);

  const bool settingsExists = !m_overridesPath.empty() && std::filesystem::exists(m_overridesPath);
  state.insert_or_assign("exists", settingsExists);
  if (settingsExists) {
    std::string readError;
    state.insert_or_assign("content", readTextFile(m_overridesPath, &readError));
    if (!readError.empty()) {
      state.insert_or_assign("read_error", readError);
    } else {
      try {
        auto table = toml::parse_file(m_overridesPath);
        deepMerge(merged, table);
      } catch (const toml::parse_error& e) {
        state.insert_or_assign("parse_error", e.what());
      }
    }
  } else {
    state.insert_or_assign("content", "");
  }
  root.insert_or_assign("state_settings", std::move(state));

  toml::table mergedConfig;
  mergedConfig.insert_or_assign("content", formatToml(merged));
  root.insert_or_assign("merged_config", std::move(mergedConfig));

  return formatToml(root) + "\n";
}
