#include "config/config_validate.h"

#include "config/config_merge.h"
#include "config/config_migrations.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_sections.h"
#include "config/schema/engine.h"
#include "config/widget_config.h"
#include "launcher/launcher_provider.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "shell/bar/widget_action.h"
#include "shell/bar/widget_gesture.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "shell/settings/widget_settings_registry.h"
#include "system/day_night_schedule.h"
#include "time/time_format.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noctalia::config {
  namespace {

    std::string formatBound(double value) {
      if (value == std::floor(value) && std::abs(value) < 1.0e15) {
        return std::to_string(static_cast<std::int64_t>(value));
      }
      return std::to_string(value);
    }

    std::string formatParseError(const std::filesystem::path& file, const toml::parse_error& e) {
      const auto& pos = e.source().begin;
      return file.string()
          + ":"
          + std::to_string(pos.line)
          + ":"
          + std::to_string(pos.column)
          + ": "
          + std::string(e.description());
    }

    // Merge config-dir *.toml (honoring [include]) then the state-dir settings.toml,
    // mirroring loadAll's order. Syntax / missing-include errors are recorded and
    // merging continues with what parsed.
    toml::table mergeSources(
        std::string_view configDir, std::string_view settingsTomlPath, schema::Diagnostics& diag,
        std::vector<std::filesystem::path>& loadedFilesOut
    ) {
      auto mergeResult = mergeConfigWithIncludes(configDir);
      toml::table merged = std::move(mergeResult.merged);
      loadedFilesOut = std::move(mergeResult.loadedFiles);
      if (!mergeResult.firstError.empty()) {
        diag.fatal("syntax", mergeResult.firstError, "config.syntax");
      }
      if (!settingsTomlPath.empty() && std::filesystem::exists(settingsTomlPath)) {
        try {
          toml::table sidecar = toml::parse_file(std::string(settingsTomlPath));
          if (const auto version = storedConfigVersion(sidecar, diag); version.has_value()) {
            const int appliedVersion = applyPendingConfigMigrations(sidecar, *version, diag);
            sidecar.insert_or_assign(kConfigVersionKey, static_cast<std::int64_t>(appliedVersion));
          }
          ConfigService::deepMerge(merged, sidecar);
        } catch (const toml::parse_error& e) {
          diag.fatal("syntax", formatParseError(settingsTomlPath, e), "config.syntax");
        }
      }
      merged.erase(kConfigVersionKey);
      LegacyConfigIssues issues;
      normalizeLegacyConfig(merged, issues);
      for (const LegacyConfigIssue& issue : issues) {
        diag.warn(issue.path, issue.message);
      }
      return merged;
    }

    // custom_schedule promises a sunset/sunrise schedule. If the times cannot deliver one,
    // say so instead of silently scheduling by coordinates (or not at all).
    void validateLocation(const toml::table& merged, schema::Diagnostics& diag) {
      const auto* location = merged["location"].as_table();
      if (location == nullptr) {
        return;
      }

      const bool customSchedule = (*location)["custom_schedule"].value_or(false);
      for (const std::string_view key : {"sunset", "sunrise"}) {
        const std::string path = "location." + std::string(key);
        const auto value = (*location)[key].value<std::string>();
        const bool set = value.has_value() && !value->empty();
        if (set && !day_night_schedule::normalizedClock(*value).has_value()) {
          const std::string message = "\"" + *value + "\" is not a time of day in HH:MM form";
          if (customSchedule) {
            diag.error(path, message, "location.clock.invalid");
          } else {
            diag.warn(path, message);
          }
        } else if (!set && customSchedule) {
          diag.error(
              path, "custom_schedule needs a " + std::string(key) + " time in HH:MM form", "location.clock.missing"
          );
        }
      }
    }

    // Launcher provider tables are keyed by provider id, which the schema can't enumerate.
    // Warn on an empty common prefix, on providers that name nothing real (or a disabled
    // plugin, or the fixed Applications provider), and on two providers sharing a prefix.
    void validateLauncherProviders(const toml::table& merged, schema::Diagnostics& diag) {
      const auto* shellTbl = merged["shell"].as_table();
      const auto* launcherTbl = shellTbl != nullptr ? (*shellTbl)["launcher"].as_table() : nullptr;
      if (launcherTbl == nullptr) {
        return;
      }
      if (const auto* prefixVal = (*launcherTbl)["provider_prefix"].as_string()) {
        if (prefixVal->get().empty()) {
          diag.warn("shell.launcher.provider_prefix", "is empty, falling back to '/'");
        }
      }
      const auto* providersTbl = (*launcherTbl)["providers"].as_table();
      if (providersTbl == nullptr) {
        return;
      }
      std::unordered_map<std::string, std::string> seenPrefixes;
      std::unordered_set<std::string> enabledPlugins;
      if (const auto* pluginsTbl = merged["plugins"].as_table()) {
        if (const auto* enabledArr = (*pluginsTbl)["enabled"].as_array()) {
          for (const auto& node : *enabledArr) {
            if (const auto* strVal = node.as_string()) {
              enabledPlugins.insert(StringUtils::toLower(strVal->get()));
            }
          }
        }
      }
      for (const auto& [key, node] : *providersTbl) {
        const auto* provTbl = node.as_table();
        if (provTbl == nullptr) {
          continue;
        }
        std::string providerName = StringUtils::toLower(std::string(key.str()));
        if (providerName == "applications") {
          diag.warn(
              "shell.launcher.providers.applications", "custom settings are not allowed (Applications is always global)"
          );
          continue;
        }
        const auto isBuiltin = std::ranges::contains(launcher::kBuiltinProviders, providerName);
        if (!isBuiltin) {
          const std::size_t colon = providerName.find(':');
          bool isPlugin = false;
          std::string pluginIdStr;
          if (colon != std::string::npos) {
            std::string_view pluginId = std::string_view(providerName).substr(0, colon);
            std::string_view entryName = std::string_view(providerName).substr(colon + 1);
            if (scripting::isValidPluginId(pluginId) && scripting::isValidPluginIdSegment(entryName)) {
              isPlugin = true;
              pluginIdStr = std::string(pluginId);
            }
          }
          if (!isPlugin) {
            diag.warn("shell.launcher.providers." + providerName, "provider is nonexistent");
            continue;
          }
          if (!enabledPlugins.contains(pluginIdStr)) {
            diag.warn("shell.launcher.providers." + providerName, "plugin '" + pluginIdStr + "' is not enabled");
            continue;
          }
        }
        const auto* prefixVal = (*provTbl)["prefix"].as_string();
        if (prefixVal == nullptr || prefixVal->get().empty()) {
          continue;
        }
        auto [it, inserted] = seenPrefixes.emplace(prefixVal->get(), std::string(key.str()));
        if (!inserted) {
          diag.warn(
              "shell.launcher.providers." + std::string(key.str()) + ".prefix",
              "duplicates the prefix of '" + it->second + "'; only one provider will be reachable by it"
          );
        }
      }
    }

    // Shape-checks a file's [include] table. The merged config has [include]
    // stripped, so this runs on raw per-file tables (single-file validate, and the
    // per-loaded-file pass in validateConfigSources).
    void validateIncludeShape(const toml::table& tbl, schema::Diagnostics& diag) {
      if (!tbl.contains("include")) {
        return;
      }
      const auto* inc = tbl["include"].as_table();
      if (inc == nullptr) {
        diag.fatal("include", "[include] must be a table", "config.include.type");
        return;
      }
      for (const auto& [key, node] : *inc) {
        const std::string k(key.str());
        if (k == "autoload") {
          if (!node.is_boolean()) {
            diag.fatal("include.autoload", "must be a boolean", "config.include.type");
          }
        } else if (k == "files") {
          const auto* arr = node.as_array();
          if (arr == nullptr) {
            diag.fatal("include.files", "must be an array of strings", "config.include.type");
          } else {
            for (const auto& el : *arr) {
              if (!el.is_string()) {
                diag.fatal("include.files", "every entry must be a string", "config.include.type");
                break;
              }
            }
          }
        } else {
          diag.warn("include." + k, "unknown setting");
        }
      }
    }

    // collectUnknownKeys + a read into a defaulted struct: the former flags misspelled
    // keys, the latter surfaces enum/range warnings (and throws → error on bad values).
    void checkSection(const toml::table& root, const schema::SectionSpec& spec, schema::Diagnostics& diag) {
      const auto* tbl = root[spec.name].as_table();
      if (tbl == nullptr) {
        return;
      }
      std::vector<std::string> unknown;
      spec.collectUnknown(*tbl, unknown);
      for (const auto& path : unknown) {
        if (!spec.allowUnknownPaths.contains(path)) {
          diag.warn(path, "unknown setting");
        }
      }
      try {
        spec.checkAgainstDefaults(*tbl, diag);
      } catch (const std::exception& e) {
        diag.error(std::string(spec.name), e.what());
      }
    }

    void
    rangeCheck(double value, const schema::WidgetSettingField& f, const std::string& path, schema::Diagnostics& d) {
      if (f.minValue && value < *f.minValue) {
        d.warn(path, "below minimum " + formatBound(*f.minValue));
      }
      if (f.maxValue && value > *f.maxValue) {
        d.warn(path, "above maximum " + formatBound(*f.maxValue));
      }
    }

    // Validate one widget setting value against its schema field. Type/color
    // problems are errors; out-of-range and bad-enum are advisory warnings
    // (matching the parser's clamp-and-continue behavior for known sections).
    void validateWidgetValue(
        const toml::node& node, const schema::WidgetSettingField& f, const std::string& path, schema::Diagnostics& diag,
        std::string_view componentOwner = {}
    ) {
      const auto reportError = [&](const std::string& errorPath, std::string message) {
        if (componentOwner.empty()) {
          diag.error(errorPath, std::move(message));
        } else {
          diag.componentError(errorPath, std::string(componentOwner), std::move(message));
        }
      };
      using schema::WidgetSettingType;
      switch (f.type) {
      case WidgetSettingType::Bool:
        if (!node.is_boolean()) {
          reportError(path, "expected a boolean");
        }
        break;
      case WidgetSettingType::Int:
        if (auto v = node.value<std::int64_t>()) {
          rangeCheck(static_cast<double>(*v), f, path, diag);
        } else {
          reportError(path, "expected an integer");
        }
        break;
      case WidgetSettingType::Double:
        if (auto v = node.value<double>()) {
          rangeCheck(*v, f, path, diag);
        } else {
          reportError(path, "expected a number");
        }
        break;
      case WidgetSettingType::OptionalDouble: {
        if (auto v = node.value<double>()) {
          rangeCheck(*v, f, path, diag);
        } else {
          const auto value = node.value<std::string>();
          if (!value.has_value() || *value != "auto") {
            reportError(path, "expected a number or \"auto\"");
          }
        }
        break;
      }
      case WidgetSettingType::String:
        if (!node.is_string()) {
          reportError(path, "expected a string");
        }
        break;
      case WidgetSettingType::StringList:
        if (const auto* arr = node.as_array()) {
          for (const auto& item : *arr) {
            if (!item.is_string()) {
              reportError(path, "expected a list of strings");
              break;
            }
          }
        } else {
          reportError(path, "expected a list of strings");
        }
        break;
      case WidgetSettingType::StringMap:
        if (const auto* table = node.as_table()) {
          // Gesture bindings have a closed key set and a grammar. Verb existence is checked at
          // bind-resolution time instead: there is no live IpcService here, and a second copy of
          // the command list would be a duplicate source of truth.
          const bool gestureActions = f.key == "actions";
          for (const auto& [mapKey, mapValue] : *table) {
            const std::string entryPath = path + "." + std::string(mapKey.str());
            const auto value = mapValue.value<std::string>();
            if (!value.has_value()) {
              reportError(entryPath, "expected a string");
              continue;
            }
            if (!gestureActions) {
              continue;
            }
            if (!noctalia::bar::parseGestureKey(mapKey.str()).has_value()) {
              reportError(entryPath, "unknown gesture");
              continue;
            }
            if (auto action = noctalia::bar::parseWidgetAction(*value); !action.has_value()) {
              reportError(entryPath, action.error());
            }
          }
        } else {
          reportError(path, "expected a table of strings");
        }
        break;
      case WidgetSettingType::Enum: {
        // Most selects store a string, but integer-valued ones (e.g. font_weight)
        // store an int — accept either and compare by string form.
        std::optional<std::string> value;
        if (auto v = node.value<std::string>()) {
          value = *v;
        } else if (auto i = node.value<std::int64_t>()) {
          value = std::to_string(*i);
        }
        if (!value) {
          reportError(path, "expected a string or integer");
        } else if (!std::ranges::contains(f.enumValues, *value)) {
          diag.warn(path, "\"" + *value + "\" is not one of the allowed values");
        }
        break;
      }
      case WidgetSettingType::Color:
        if (auto v = node.value<std::string>()) {
          try {
            (void)colorSpecFromConfigString(*v); // empty context: diag already carries the path
          } catch (const std::exception& e) {
            reportError(path, e.what());
          }
        } else {
          reportError(path, "expected a string color");
        }
        break;
      }
    }

    // Validate a free-form setting map (bar/desktop widget settings) against a
    // per-type schema. Unknown keys are errors only for `flagUnknown` types
    // (built-in, non-scripted); scripted widgets carry user-defined settings.
    void validateSettingsMap(
        const toml::table& settings, const schema::WidgetSettingSchema& fields, const std::string& base,
        bool flagUnknown, schema::Diagnostics& diag, const std::unordered_set<std::string>& ignoreKeys = {},
        std::string_view componentOwner = {}
    ) {
      for (const auto& [key, node] : settings) {
        const std::string keyStr(key.str());
        if (ignoreKeys.contains(keyStr)) {
          continue;
        }
        const auto field = std::ranges::find(fields, keyStr, &schema::WidgetSettingField::key);
        if (field == fields.end()) {
          if (flagUnknown) {
            diag.warn(base + "." + keyStr, "unknown setting");
          }
          continue;
        }
        validateWidgetValue(node, *field, base + "." + keyStr, diag, componentOwner);
      }
    }

    void validateCalendarSyntax(const toml::table& root, schema::Diagnostics& diag) {
      const auto* calendar = root["calendar"].as_table();
      if (calendar == nullptr) {
        return;
      }
      if ((*calendar)["accounts"].as_array() != nullptr) {
        diag.error("calendar.accounts", "calendar accounts now use [calendar.account.<id>] named tables");
      }
      const auto* accounts = (*calendar)["account"].as_table();
      if (accounts == nullptr) {
        return;
      }
      for (const auto& [id, node] : *accounts) {
        const auto* account = node.as_table();
        if (account == nullptr || !account->contains("url")) {
          continue;
        }
        diag.error(
            "calendar.account." + std::string(id.str()) + ".url",
            "CalDAV collection url was removed; use provider/server_url discovery syntax instead"
        );
      }
    }

    void validatePluginSettings(
        const toml::table& root, schema::Diagnostics& diag, scripting::PluginRegistry& pluginRegistry
    ) {
      const auto* pluginSettings = root["plugin_settings"].as_table();
      if (pluginSettings == nullptr) {
        return;
      }
      for (const auto& [pluginId, node] : *pluginSettings) {
        const auto* perPlugin = node.as_table();
        if (perPlugin == nullptr) {
          continue;
        }
        const std::string idStr(pluginId.str());
        const std::string base = "plugin_settings." + idStr;
        const scripting::PluginManifest* manifest = pluginRegistry.findManifest(idStr);
        if (manifest == nullptr) {
          diag.warn(base, "no loaded plugin with this id");
          continue;
        }
        schema::WidgetSettingSchema fields;
        for (const auto& spec : settings::manifestSettingSpecs(manifest->settings)) {
          fields.push_back(spec.schema);
        }
        for (const auto& entry : manifest->entries) {
          if (entry.kind != scripting::PluginEntryKind::Panel) {
            continue;
          }
          for (const auto& spec : settings::pluginPanelShellSettingSpecs(entry)) {
            fields.push_back(spec.schema);
          }
          for (const auto& spec : settings::manifestSettingSpecs(entry.settings)) {
            if (scripting::isPanelShellSettingKey(entry.id, spec.schema.key)) {
              continue;
            }
            fields.push_back(spec.schema);
          }
        }
        validateSettingsMap(*perPlugin, fields, base, /*flagUnknown=*/true, diag);
      }
    }

    void
    validateBarWidgets(const toml::table& root, schema::Diagnostics& diag, scripting::PluginRegistry& pluginRegistry) {
      const auto* widgets = root["widget"].as_table();
      if (widgets == nullptr) {
        return;
      }
      Config resolvedConfig;
      seedBuiltinWidgets(resolvedConfig);
      for (const auto& [name, node] : *widgets) {
        const auto* tbl = node.as_table();
        if (tbl == nullptr) {
          continue;
        }
        const std::string nameStr(name.str());
        const std::string base = "widget." + nameStr;
        WidgetConfig wc = readBarWidgetConfig(nameStr, *tbl, resolvedConfig);
        const std::string type = wc.type;
        const auto pluginEntry = pluginRegistry.resolve(type);
        const bool isPluginWidget =
            pluginEntry.has_value() && pluginEntry->entry->kind == scripting::PluginEntryKind::Widget;
        if (!settings::isBuiltInWidgetType(type) && !isPluginWidget) {
          diag.warn(base, "unrecognized widget type \"" + type + "\"");
          resolvedConfig.widgets[nameStr] = std::move(wc);
          continue;
        }
        const auto fields = settings::widgetSettingSchema(type, &wc, &pluginRegistry);
        // Plugin widgets resolve their settings from a static plugin.toml manifest, so
        // unknown keys are flagged like any other widget.
        validateSettingsMap(*tbl, fields, base, /*flagUnknown=*/true, diag, /*ignoreKeys=*/{"type"}, base);
        if (auto error = settings::validateWidgetSemantics(type, &wc); error.has_value()) {
          diag.componentError(base, base, *error);
        }
        if (type == "clock") {
          if (const auto timezone = (*tbl)["timezone"].value<std::string>();
              timezone.has_value() && !isValidTimezone(*timezone)) {
            diag.componentError(
                base + ".timezone", base, "unknown timezone \"" + *timezone + "\"", "clock.timezone.unknown"
            );
          }
        }
        resolvedConfig.widgets[nameStr] = std::move(wc);
      }
    }

    void validateDesktopWidgets(
        const toml::table& root, schema::Diagnostics& diag, scripting::PluginRegistry& pluginRegistry
    ) {
      const auto* dw = root["desktop_widgets"].as_table();
      if (dw == nullptr) {
        return;
      }
      static const std::unordered_set<std::string> kTopLevel = {
          "enabled", "schema_version", "grid", "widget", "widget_order"
      };
      for (const auto& [key, node] : *dw) {
        (void)node;
        if (!kTopLevel.contains(std::string(key.str()))) {
          diag.warn("desktop_widgets." + std::string(key.str()), "unknown setting");
        }
      }
      if (const auto* grid = (*dw)["grid"].as_table()) {
        static const std::unordered_set<std::string> kGrid = {
            "visible",
            "cell_size",
            "major_interval",
        };
        for (const auto& [key, node] : *grid) {
          (void)node;
          if (!kGrid.contains(std::string(key.str()))) {
            diag.warn("desktop_widgets.grid." + std::string(key.str()), "unknown setting");
          }
        }
      }
      const auto* widgets = (*dw)["widget"].as_table();
      if (widgets == nullptr) {
        return;
      }
      static const std::unordered_set<std::string> kWidgetKeys = {"id",     "type",      "output",     "cx",
                                                                  "cy",     "box_width", "box_height", "rotation",
                                                                  "flip_x", "flip_y",    "enabled",    "settings"};
      for (const auto& [id, node] : *widgets) {
        const auto* tbl = node.as_table();
        if (tbl == nullptr) {
          continue;
        }
        const std::string idStr(id.str());
        const std::string base = "desktop_widgets.widget." + idStr;
        for (const auto& [key, value] : *tbl) {
          (void)value;
          if (!kWidgetKeys.contains(std::string(key.str()))) {
            diag.warn(base + "." + std::string(key.str()), "unknown setting");
          }
        }
        const std::string type = (*tbl)["type"].value<std::string>().value_or("");
        // A known type contributes at least one type-specific spec; unknown ones
        // only get the shared background settings, so detect via the type-only list.
        const bool isBuiltIn =
            std::ranges::any_of(desktop_settings::desktopWidgetTypeSpecs(), [type](const auto& spec) {
              return spec.type == type;
            });
        const auto pluginEntry = pluginRegistry.resolve(type);
        const bool isPluginWidget =
            pluginEntry.has_value() && pluginEntry->entry->kind == scripting::PluginEntryKind::DesktopWidget;
        if (!isBuiltIn && !isPluginWidget) {
          diag.warn(base, "unrecognized desktop widget type \"" + type + "\"");
          continue;
        }
        const auto* settingsTbl = (*tbl)["settings"].as_table();
        if (settingsTbl == nullptr) {
          continue;
        }
        validateSettingsMap(
            *settingsTbl, desktop_settings::desktopWidgetSettingSchema(type, &pluginRegistry), base + ".settings",
            /*flagUnknown=*/true, diag, {}, base
        );
        if (type == "clock") {
          if (const auto timezone = (*settingsTbl)["timezone"].value<std::string>();
              timezone.has_value() && !isValidTimezone(*timezone)) {
            diag.componentError(
                base + ".settings.timezone", base, "unknown timezone \"" + *timezone + "\"", "clock.timezone.unknown"
            );
          }
        }
      }
    }

    void validateLockscreenWidgets(
        const toml::table& root, schema::Diagnostics& diag, scripting::PluginRegistry& pluginRegistry
    ) {
      const auto* section = root["lockscreen_widgets"].as_table();
      if (section == nullptr) {
        return;
      }
      static const std::unordered_set<std::string> kTopLevel = {
          "enabled", "schema_version", "grid", "widget", "widget_order"
      };
      for (const auto& [key, node] : *section) {
        (void)node;
        if (!kTopLevel.contains(std::string(key.str()))) {
          diag.warn("lockscreen_widgets." + std::string(key.str()), "unknown setting");
        }
      }
      if (const auto* grid = (*section)["grid"].as_table()) {
        static const std::unordered_set<std::string> kGrid = {
            "visible",
            "cell_size",
            "major_interval",
        };
        for (const auto& [key, node] : *grid) {
          (void)node;
          if (!kGrid.contains(std::string(key.str()))) {
            diag.warn("lockscreen_widgets.grid." + std::string(key.str()), "unknown setting");
          }
        }
      }
      const auto* widgets = (*section)["widget"].as_table();
      if (widgets == nullptr) {
        return;
      }
      static const std::unordered_set<std::string> kWidgetKeys = {"id",     "type",      "output",     "cx",
                                                                  "cy",     "box_width", "box_height", "rotation",
                                                                  "flip_x", "flip_y",    "enabled",    "settings"};
      for (const auto& [id, node] : *widgets) {
        const auto* tbl = node.as_table();
        if (tbl == nullptr) {
          continue;
        }
        const std::string idStr(id.str());
        const std::string base = "lockscreen_widgets.widget." + idStr;
        for (const auto& [key, value] : *tbl) {
          (void)value;
          if (!kWidgetKeys.contains(std::string(key.str()))) {
            diag.warn(base + "." + std::string(key.str()), "unknown setting");
          }
        }
        const std::string type = (*tbl)["type"].value<std::string>().value_or("");
        const bool isBuiltIn =
            std::ranges::any_of(desktop_settings::desktopWidgetTypeSpecs(), [type](const auto& spec) {
              return spec.type == type;
            });
        const auto pluginEntry = pluginRegistry.resolve(type);
        const bool isPluginWidget =
            pluginEntry.has_value() && pluginEntry->entry->kind == scripting::PluginEntryKind::DesktopWidget;
        if (type != lockscreen_login_box::kWidgetType && !isBuiltIn && !isPluginWidget) {
          diag.warn(base, "unrecognized lockscreen widget type \"" + type + "\"");
          continue;
        }
        const auto* settingsTbl = (*tbl)["settings"].as_table();
        if (settingsTbl == nullptr) {
          continue;
        }
        validateSettingsMap(
            *settingsTbl, desktop_settings::desktopWidgetSettingSchema(type, &pluginRegistry), base + ".settings",
            /*flagUnknown=*/true, diag, {}, base
        );
        if (type == "clock") {
          if (const auto timezone = (*settingsTbl)["timezone"].value<std::string>();
              timezone.has_value() && !isValidTimezone(*timezone)) {
            diag.componentError(
                base + ".settings.timezone", base, "unknown timezone \"" + *timezone + "\"", "clock.timezone.unknown"
            );
          }
        }
      }
    }

    void validateBars(const toml::table& root, schema::Diagnostics& diag) {
      const auto* bars = root["bar"].as_table();
      if (bars == nullptr) {
        return;
      }
      for (const auto& [name, node] : *bars) {
        if (name.str() == std::string_view("order")) {
          continue;
        }
        const auto* barTbl = node.as_table();
        if (barTbl == nullptr) {
          continue;
        }
        const std::string base = "bar." + std::string(name.str());
        std::vector<std::string> unknown;
        schema::collectUnknownKeys(*barTbl, schema::barFieldsSchema(), base, unknown);
        for (const auto& path : unknown) {
          // position + the monitor override map are handled outside barFieldsSchema.
          if (path == base + ".position" || path == base + ".monitor") {
            continue;
          }
          diag.warn(path, "unknown setting");
        }
        BarConfig tmpBar{};
        try {
          schema::readInto(*barTbl, tmpBar, schema::barFieldsSchema(), base, diag);
        } catch (const std::exception& e) {
          diag.error(base, e.what());
        }
        if (const auto* monitors = (*barTbl)["monitor"].as_table()) {
          for (const auto& [match, monNode] : *monitors) {
            const auto* monTbl = monNode.as_table();
            if (monTbl == nullptr) {
              continue;
            }
            const std::string monBase = base + ".monitor." + std::string(match.str());
            std::vector<std::string> monUnknown;
            schema::collectUnknownKeys(*monTbl, schema::barMonitorOverrideSchema(), monBase, monUnknown);
            for (const auto& path : monUnknown) {
              diag.warn(path, "unknown setting");
            }
            BarMonitorOverride tmpOvr{};
            try {
              schema::readInto(*monTbl, tmpOvr, schema::barMonitorOverrideSchema(), monBase, diag);
            } catch (const std::exception& e) {
              diag.error(monBase, e.what());
            }
          }
        }
      }
    }

    void appendMergedConfigDiagnostics(const toml::table& merged, schema::Diagnostics& diag) {
      for (const schema::SectionSpec& spec : schema::sections()) {
        checkSection(merged, spec, diag);
      }
      validateCalendarSyntax(merged, diag);
      validateLauncherProviders(merged, diag);

      // Resolve the candidate's plugin catalog without mutating the live registry.
      scripting::PluginRegistry pluginRegistry;
      {
        PluginsConfig pc;
        schema::Diagnostics sink; // schema issues are already reported by checkSection above
        if (const auto* pluginsTbl = merged["plugins"].as_table()) {
          const bool sourcesConfigured = (*pluginsTbl)["source"].as_array() != nullptr;
          schema::readInto(*pluginsTbl, pc, schema::pluginsSchema(), "plugins", sink);
          if (!sourcesConfigured && pc.sources.empty()) {
            pc.sources = defaultPluginSources();
          }
        } else {
          pc.sources = defaultPluginSources();
        }
        scripting::applyPluginSourcesToRegistry(pluginRegistry, pc);
      }

      validateLocation(merged, diag);
      validateBars(merged, diag);
      validateBarWidgets(merged, diag, pluginRegistry);
      validatePluginSettings(merged, diag, pluginRegistry);
      validateDesktopWidgets(merged, diag, pluginRegistry);
      validateLockscreenWidgets(merged, diag, pluginRegistry);
      validateIncludeShape(merged, diag);

      // Unknown top-level keys.
      for (const auto& [key, node] : merged) {
        (void)node;
        if (!schema::isKnownRootKey(key.str())) {
          diag.warn(std::string(key.str()), "unknown section");
        }
      }
    }

  } // namespace

  schema::Diagnostics validateConfigSources(std::string_view configDir, std::string_view settingsTomlPath) {
    schema::Diagnostics diag;
    std::vector<std::filesystem::path> loadedFiles;
    const toml::table merged = mergeSources(configDir, settingsTomlPath, diag, loadedFiles);
    // [include] is stripped from the merged table, so validate each loaded file's
    // raw [include] shape directly (covers root and subdirectory includes).
    for (const auto& file : loadedFiles) {
      try {
        const toml::table raw = toml::parse_file(file.string());
        validateIncludeShape(raw, diag);
      } catch (const toml::parse_error&) {
        // Syntax errors were already reported during the merge.
      }
    }
    appendMergedConfigDiagnostics(merged, diag);
    return diag;
  }

  schema::Diagnostics validateConfigFile(std::string_view path) {
    schema::Diagnostics diag;
    toml::table parsed;
    try {
      parsed = toml::parse_file(std::string(path));
    } catch (const toml::parse_error& e) {
      diag.fatal("syntax", formatParseError(std::filesystem::path(std::string(path)), e), "config.syntax");
      return diag;
    }

    LegacyConfigIssues issues;
    normalizeLegacyConfig(parsed, issues);
    for (const LegacyConfigIssue& issue : issues) {
      diag.warn(issue.path, issue.message);
    }
    appendMergedConfigDiagnostics(parsed, diag);
    return diag;
  }

  schema::Diagnostics validateMergedConfig(const toml::table& merged) {
    schema::Diagnostics diag;
    appendMergedConfigDiagnostics(merged, diag);
    return diag;
  }

} // namespace noctalia::config
