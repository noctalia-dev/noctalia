#include "config/config_validate.h"

#include "config/config_merge.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "config/widget_config.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "shell/settings/widget_settings_registry.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
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
        diag.error("syntax", mergeResult.firstError);
      }
      if (!settingsTomlPath.empty() && std::filesystem::exists(settingsTomlPath)) {
        try {
          toml::table parsed = toml::parse_file(std::string(settingsTomlPath));
          ConfigService::deepMerge(merged, parsed);
        } catch (const toml::parse_error& e) {
          diag.error("syntax", formatParseError(settingsTomlPath, e));
        }
      }
      return merged;
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
        diag.error("include", "[include] must be a table");
        return;
      }
      for (const auto& [key, node] : *inc) {
        const std::string k(key.str());
        if (k == "autoload") {
          if (!node.is_boolean()) {
            diag.error("include.autoload", "must be a boolean");
          }
        } else if (k == "files") {
          const auto* arr = node.as_array();
          if (arr == nullptr) {
            diag.error("include.files", "must be an array of strings");
          } else {
            for (const auto& el : *arr) {
              if (!el.is_string()) {
                diag.error("include.files", "every entry must be a string");
                break;
              }
            }
          }
        } else {
          diag.warn("include." + k, "unknown setting");
        }
      }
    }

    // collectUnknownKeys + a defaulted readInto: the former flags misspelled keys,
    // the latter surfaces enum/range warnings (and throws → error on bad values).
    template <typename T>
    void checkSection(
        const toml::table& root, std::string_view name, const schema::Schema<T>& sch, schema::Diagnostics& diag,
        const std::unordered_set<std::string>& allowUnknownPaths = {}
    ) {
      const auto* tbl = root[name].as_table();
      if (tbl == nullptr) {
        return;
      }
      std::vector<std::string> unknown;
      schema::collectUnknownKeys(*tbl, sch, name, unknown);
      for (const auto& path : unknown) {
        if (!allowUnknownPaths.contains(path)) {
          diag.warn(path, "unknown setting");
        }
      }
      T tmp{};
      try {
        schema::readInto(*tbl, tmp, sch, name, diag);
      } catch (const std::exception& e) {
        diag.error(std::string(name), e.what());
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
        const toml::node& node, const schema::WidgetSettingField& f, const std::string& path, schema::Diagnostics& diag
    ) {
      using schema::WidgetSettingType;
      switch (f.type) {
      case WidgetSettingType::Bool:
        if (!node.is_boolean()) {
          diag.error(path, "expected a boolean");
        }
        break;
      case WidgetSettingType::Int:
        if (auto v = node.value<std::int64_t>()) {
          rangeCheck(static_cast<double>(*v), f, path, diag);
        } else {
          diag.error(path, "expected an integer");
        }
        break;
      case WidgetSettingType::Double:
      case WidgetSettingType::OptionalDouble:
        if (auto v = node.value<double>()) {
          rangeCheck(*v, f, path, diag);
        } else {
          diag.error(path, "expected a number");
        }
        break;
      case WidgetSettingType::String:
        if (!node.is_string()) {
          diag.error(path, "expected a string");
        }
        break;
      case WidgetSettingType::StringList:
        if (const auto* arr = node.as_array()) {
          for (const auto& item : *arr) {
            if (!item.is_string()) {
              diag.error(path, "expected a list of strings");
              break;
            }
          }
        } else {
          diag.error(path, "expected a list of strings");
        }
        break;
      case WidgetSettingType::StringMap:
        if (const auto* table = node.as_table()) {
          for (const auto& [mapKey, mapValue] : *table) {
            if (!mapValue.is_string()) {
              diag.error(path + "." + std::string(mapKey.str()), "expected a string");
            }
          }
        } else {
          diag.error(path, "expected a table of strings");
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
          diag.error(path, "expected a string or integer");
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
            diag.error(path, e.what());
          }
        } else {
          diag.error(path, "expected a string color");
        }
        break;
      }
    }

    // Validate a free-form setting map (bar/desktop widget settings) against a
    // per-type schema. Unknown keys are errors only for `flagUnknown` types
    // (built-in, non-scripted); scripted widgets carry user-defined settings.
    void validateSettingsMap(
        const toml::table& settings, const schema::WidgetSettingSchema& fields, const std::string& base,
        bool flagUnknown, schema::Diagnostics& diag, const std::unordered_set<std::string>& ignoreKeys = {}
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
        validateWidgetValue(node, *field, base + "." + keyStr, diag);
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

    void validatePluginSettings(const toml::table& root, schema::Diagnostics& diag) {
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
        const scripting::PluginManifest* manifest = scripting::PluginRegistry::instance().findManifest(idStr);
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

    void validateBarWidgets(const toml::table& root, schema::Diagnostics& diag) {
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
        if (!settings::isBuiltInWidgetType(type) && !settings::isPluginWidgetType(type)) {
          diag.warn(base, "unrecognized widget type \"" + type + "\"");
          resolvedConfig.widgets[nameStr] = std::move(wc);
          continue;
        }
        const auto fields = settings::widgetSettingSchema(type, &wc);
        // Plugin widgets resolve their settings from a static plugin.toml manifest, so
        // unknown keys are flagged like any other widget.
        validateSettingsMap(*tbl, fields, base, /*flagUnknown=*/true, diag, /*ignoreKeys=*/{"type"});
        resolvedConfig.widgets[nameStr] = std::move(wc);
      }
    }

    void validateDesktopWidgets(const toml::table& root, schema::Diagnostics& diag) {
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
        if (desktop_settings::desktopWidgetSettingSpecs(type).empty()) {
          diag.warn(base, "unrecognized desktop widget type \"" + type + "\"");
          continue;
        }
        const auto* settingsTbl = (*tbl)["settings"].as_table();
        if (settingsTbl == nullptr) {
          continue;
        }
        validateSettingsMap(
            *settingsTbl, desktop_settings::desktopWidgetSettingSchema(type), base + ".settings",
            /*flagUnknown=*/true, diag
        );
      }
    }

    void validateLockscreenWidgets(const toml::table& root, schema::Diagnostics& diag) {
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
        if (type != lockscreen_login_box::kWidgetType && desktop_settings::desktopWidgetSettingSpecs(type).empty()) {
          diag.warn(base, "unrecognized lockscreen widget type \"" + type + "\"");
          continue;
        }
        const auto* settingsTbl = (*tbl)["settings"].as_table();
        if (settingsTbl == nullptr) {
          continue;
        }
        validateSettingsMap(
            *settingsTbl, desktop_settings::desktopWidgetSettingSchema(type), base + ".settings",
            /*flagUnknown=*/true, diag
        );
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

    void validateMergedConfig(const toml::table& merged, schema::Diagnostics& diag) {
      checkSection(merged, "shell", schema::shellSchema(), diag);
      checkSection(
          merged, "wallpaper", schema::wallpaperSchema(), diag,
          {"wallpaper.default", "wallpaper.last", "wallpaper.monitors", "wallpaper.favorite"}
      );
      checkSection(merged, "theme", schema::themeSchema(), diag);
      checkSection(merged, "backdrop", schema::backdropSchema(), diag);
      checkSection(merged, "lockscreen", schema::lockscreenSchema(), diag);
      checkSection(merged, "notification", schema::notificationSchema(), diag);
      checkSection(merged, "notifications", schema::notificationSchema(), diag); // compatibility alias
      checkSection(merged, "osd", schema::osdSchema(), diag);
      checkSection(merged, "system", schema::systemSchema(), diag);
      checkSection(merged, "weather", schema::weatherSchema(), diag);
      checkSection(merged, "calendar", schema::calendarSchema(), diag);
      validateCalendarSyntax(merged, diag);
      checkSection(merged, "audio", schema::audioSchema(), diag);
      checkSection(merged, "brightness", schema::brightnessSchema(), diag);
      checkSection(merged, "battery", schema::batterySchema(), diag);
      checkSection(merged, "nightlight", schema::nightlightSchema(), diag);
      checkSection(merged, "location", schema::locationSchema(), diag);
      checkSection(merged, "idle", schema::idleSchema(), diag);
      checkSection(merged, "keybinds", schema::keybindsSchema(), diag);
      checkSection(merged, "dock", schema::dockSchema(), diag);
      checkSection(merged, "control_center", schema::controlCenterSchema(), diag);
      checkSection(merged, "plugins", schema::pluginsSchema(), diag);
      checkSection(merged, "hooks", schema::hooksSchema(), diag);

      // Resolve [plugins] into the registry so plugin widget types validate the same
      // way the running app sees them. Disk-only — no materialization/network here.
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
        scripting::applyPluginSourcesToRegistry(scripting::PluginRegistry::instance(), pc);
      }

      validateBars(merged, diag);
      validateBarWidgets(merged, diag);
      validatePluginSettings(merged, diag);
      validateDesktopWidgets(merged, diag);
      validateLockscreenWidgets(merged, diag);
      validateIncludeShape(merged, diag);

      // Unknown top-level sections.
      static const std::unordered_set<std::string> kKnownSections = {
          "shell",
          "wallpaper",
          "theme",
          "backdrop",
          "lockscreen",
          "notification",
          "notifications",
          "osd",
          "system",
          "weather",
          "calendar",
          "audio",
          "brightness",
          "battery",
          "nightlight",
          "location",
          "idle",
          "keybinds",
          "bar",
          "dock",
          "desktop_widgets",
          "lockscreen_widgets",
          "widget",
          "control_center",
          "plugins",
          "plugin_settings",
          "hooks",
          "include",
      };
      for (const auto& [key, node] : merged) {
        (void)node;
        if (!kKnownSections.contains(std::string(key.str()))) {
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
    validateMergedConfig(merged, diag);
    return diag;
  }

  schema::Diagnostics validateConfigFile(std::string_view path) {
    schema::Diagnostics diag;
    toml::table parsed;
    try {
      parsed = toml::parse_file(std::string(path));
    } catch (const toml::parse_error& e) {
      diag.error("syntax", formatParseError(std::filesystem::path(std::string(path)), e));
      return diag;
    }

    validateMergedConfig(parsed, diag);
    return diag;
  }

} // namespace noctalia::config
