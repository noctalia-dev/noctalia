#include "scripting/plugin_manifest.h"

#include "core/log.h"
#include "core/toml.h" // IWYU pragma: keep
#include "scripting/plugin_id.h"
#include "scripting/plugin_panel_shell.h"

#include <algorithm>
#include <array>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace scripting {

  namespace {

    constexpr Logger kLog("plugin-manifest");

    std::optional<double> numericSetting(const WidgetSettingValue& value) {
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*i);
      }
      if (const auto* d = std::get_if<double>(&value)) {
        return *d;
      }
      return std::nullopt;
    }

    bool valueEqual(const WidgetSettingValue& a, const WidgetSettingValue& b) {
      const auto aNum = numericSetting(a);
      const auto bNum = numericSetting(b);
      if (aNum.has_value() || bNum.has_value()) {
        return aNum.has_value() && bNum.has_value() && *aNum == *bNum;
      }
      if (a.index() != b.index()) {
        return false;
      }
      return std::visit(
          [&](const auto& av) {
            using T = std::decay_t<decltype(av)>;
            const auto* bv = std::get_if<T>(&b);
            return bv != nullptr && av == *bv;
          },
          a
      );
    }

    // Each entry kind paired with its TOML array-table name.
    constexpr std::array<std::pair<PluginEntryKind, std::string_view>, 6> kEntryKinds{{
        {PluginEntryKind::Widget, "widget"},
        {PluginEntryKind::Panel, "panel"},
        {PluginEntryKind::Shortcut, "shortcut"},
        {PluginEntryKind::DesktopWidget, "desktop_widget"},
        {PluginEntryKind::LauncherProvider, "launcher_provider"},
        {PluginEntryKind::Service, "service"},
    }};

    ManifestFieldType parseFieldType(std::string_view type) {
      if (type == "bool" || type == "boolean") {
        return ManifestFieldType::Bool;
      }
      if (type == "int" || type == "integer") {
        return ManifestFieldType::Int;
      }
      if (type == "double" || type == "number" || type == "float") {
        return ManifestFieldType::Double;
      }
      if (type == "string_list") {
        return ManifestFieldType::StringList;
      }
      if (type == "select" || type == "enum") {
        return ManifestFieldType::Select;
      }
      if (type == "file") {
        return ManifestFieldType::File;
      }
      if (type == "folder") {
        return ManifestFieldType::Folder;
      }
      if (type == "glyph") {
        return ManifestFieldType::Glyph;
      }
      if (type == "color") {
        return ManifestFieldType::Color;
      }
      return ManifestFieldType::String;
    }

    std::string tableString(const toml::table& tbl, std::string_view key, std::string fallback = {}) {
      if (auto value = tbl[key].value<std::string>()) {
        return *value;
      }
      return fallback;
    }

    bool tableBool(const toml::table& tbl, std::string_view key, bool fallback) {
      return tbl[key].value<bool>().value_or(fallback);
    }

    std::vector<std::string> tableStringArray(const toml::table& tbl, std::string_view key) {
      std::vector<std::string> out;
      const auto* values = tbl[key].as_array();
      if (values == nullptr) {
        return out;
      }
      for (const auto& node : *values) {
        if (auto value = node.value<std::string>()) {
          out.push_back(*value);
        }
      }
      return out;
    }

    // A TOML number written as either an integer or a float.
    std::optional<double> tableNumber(const toml::table& tbl, std::string_view key) {
      const auto node = tbl[key];
      if (auto i = node.value<std::int64_t>()) {
        return static_cast<double>(*i);
      }
      if (auto d = node.value<double>()) {
        return d;
      }
      return std::nullopt;
    }

    void parseFieldDefault(const toml::table& field, ManifestField& out) {
      const auto node = field["default"];
      switch (out.type) {
      case ManifestFieldType::Bool:
        out.boolDefault = node.value<bool>().value_or(false);
        break;
      case ManifestFieldType::Int:
      case ManifestFieldType::Double:
        if (auto i = node.value<std::int64_t>()) {
          out.numberDefault = static_cast<double>(*i);
        } else {
          out.numberDefault = node.value<double>().value_or(0.0);
        }
        break;
      case ManifestFieldType::StringList:
        if (const auto* values = node.as_array()) {
          for (const auto& valueNode : *values) {
            if (auto value = valueNode.value<std::string>()) {
              out.stringListDefault.push_back(*value);
            }
          }
        }
        break;
      default:
        out.stringDefault = node.value<std::string>().value_or(std::string{});
        break;
      }
    }

    bool parseFieldOptions(const toml::table& field, ManifestField& out, std::string& error) {
      const auto* options = field["options"].as_array();
      if (options == nullptr) {
        return true;
      }
      for (const auto& node : *options) {
        if (const auto* optTable = node.as_table()) {
          ManifestSelectOption opt;
          opt.value = tableString(*optTable, "value");
          opt.label = tableString(*optTable, "label");
          opt.labelKey = tableString(*optTable, "label_key");
          if (!opt.label.empty() && !opt.labelKey.empty()) {
            error = "setting '" + out.key + "' option '" + opt.value + "' declares both label and label_key";
            return false;
          }
          if (opt.label.empty() && opt.labelKey.empty()) {
            opt.label = opt.value;
          }
          if (!opt.value.empty()) {
            out.options.push_back(std::move(opt));
          }
        } else if (auto value = node.value<std::string>()) {
          out.options.push_back(ManifestSelectOption{.value = *value, .label = *value, .labelKey = {}});
        }
      }
      return true;
    }

    void parseFieldExtensions(const toml::table& field, ManifestField& out) {
      const auto* extensions = field["extensions"].as_array();
      if (extensions == nullptr) {
        return;
      }
      for (const auto& node : *extensions) {
        if (auto value = node.value<std::string>()) {
          out.extensions.push_back(*value);
        }
      }
    }

    void parseFieldVisibility(const toml::table& field, ManifestField& out) {
      const auto* visible = field["visible_when"].as_table();
      if (visible == nullptr) {
        return;
      }
      ManifestVisibility vis;
      vis.key = tableString(*visible, "key");
      if (const auto* values = (*visible)["values"].as_array()) {
        for (const auto& node : *values) {
          if (auto value = node.value<std::string>()) {
            vis.values.push_back(*value);
          } else if (auto boolean = node.value<bool>()) {
            vis.values.emplace_back(*boolean ? "true" : "false");
          }
        }
      }
      if (!vis.key.empty() && !vis.values.empty()) {
        out.visibleWhen = std::move(vis);
      }
    }

    std::optional<ManifestField> parseField(const toml::table& field, std::string& error) {
      ManifestField out;
      out.key = tableString(field, "key");
      if (out.key.empty()) {
        return out;
      }
      out.type = parseFieldType(tableString(field, "type", "string"));
      out.label = tableString(field, "label");
      out.labelKey = tableString(field, "label_key");
      if (!out.label.empty() && !out.labelKey.empty()) {
        error = "setting '" + out.key + "' declares both label and label_key";
        return std::nullopt;
      }
      out.description = tableString(field, "description");
      out.descriptionKey = tableString(field, "description_key");
      if (!out.description.empty() && !out.descriptionKey.empty()) {
        error = "setting '" + out.key + "' declares both description and description_key";
        return std::nullopt;
      }
      out.advanced = tableBool(field, "advanced", false);
      out.minValue = tableNumber(field, "min");
      out.maxValue = tableNumber(field, "max");
      if (auto step = tableNumber(field, "step")) {
        out.step = *step;
      }
      parseFieldDefault(field, out);
      if (!parseFieldOptions(field, out, error)) {
        return std::nullopt;
      }
      parseFieldExtensions(field, out);
      parseFieldVisibility(field, out);
      return out;
    }

    bool parseEntries(
        const toml::table& root, PluginEntryKind kind, std::string_view tableName, PluginManifest& manifest,
        std::string& error
    ) {
      const auto* entries = root[tableName].as_array();
      if (entries == nullptr) {
        return true;
      }
      for (const auto& node : *entries) {
        const auto* entryTable = node.as_table();
        if (entryTable == nullptr) {
          continue;
        }
        PluginEntry entry;
        entry.kind = kind;
        entry.id = tableString(*entryTable, "id");
        entry.entry = tableString(*entryTable, "entry");
        if (entry.id.empty()) {
          continue;
        }
        if (const auto* settings = (*entryTable)["setting"].as_array()) {
          for (const auto& settingNode : *settings) {
            if (const auto* settingTable = settingNode.as_table()) {
              auto field = parseField(*settingTable, error);
              if (!field.has_value()) {
                return false;
              }
              if (!field->key.empty()) {
                entry.settings.push_back(std::move(*field));
              }
            }
          }
        }
        if (kind == PluginEntryKind::Panel) {
          entry.panelWidth = std::max(0.0, tableNumber(*entryTable, "width").value_or(0.0));
          entry.panelHeight = std::max(0.0, tableNumber(*entryTable, "height").value_or(0.0));
          if (const std::string placement = tableString(*entryTable, "placement"); !placement.empty()) {
            entry.panelPlacementDefault = placement;
          }
          if (const std::string position = tableString(*entryTable, "position"); !position.empty()) {
            entry.panelPositionDefault = position;
          }
          if (const auto* openNearClick = (*entryTable)["open_near_click"].as_boolean()) {
            entry.panelOpenNearClickDefault = openNearClick->get();
          }
          injectStandardPanelShellSettings(entry);
        }
        if (kind == PluginEntryKind::LauncherProvider) {
          entry.launcherPrefix = tableString(*entryTable, "prefix");
          entry.launcherGlyph = tableString(*entryTable, "glyph");
          entry.launcherGlobalSearch = tableBool(*entryTable, "include_in_global_search", false);
          entry.launcherDebounceMs =
              std::max(0, static_cast<int>(tableNumber(*entryTable, "debounce_ms").value_or(0.0)));
          if (const auto* cats = (*entryTable)["category"].as_array()) {
            for (const auto& catNode : *cats) {
              if (const auto* catTable = catNode.as_table()) {
                ManifestLauncherCategory cat;
                cat.label = tableString(*catTable, "label");
                cat.glyph = tableString(*catTable, "glyph");
                if (!cat.label.empty()) {
                  entry.launcherCategories.push_back(std::move(cat));
                }
              }
            }
          }
        }
        manifest.entries.push_back(std::move(entry));
      }
      return true;
    }

  } // namespace

  WidgetSettingValue ManifestField::defaultValue() const {
    switch (type) {
    case ManifestFieldType::Bool:
      return WidgetSettingValue{boolDefault};
    case ManifestFieldType::Int:
      return WidgetSettingValue{static_cast<std::int64_t>(numberDefault)};
    case ManifestFieldType::Double:
      return WidgetSettingValue{numberDefault};
    case ManifestFieldType::StringList:
      return WidgetSettingValue{stringListDefault};
    default:
      return WidgetSettingValue{stringDefault};
    }
  }

  const PluginEntry* PluginManifest::findEntry(std::string_view entryId) const {
    const auto it = std::ranges::find(entries, entryId, &PluginEntry::id);
    return it != entries.end() ? &*it : nullptr;
  }

  std::string_view pluginEntryTableName(PluginEntryKind kind) {
    for (const auto& [k, name] : kEntryKinds) {
      if (k == kind) {
        return name;
      }
    }
    return {};
  }

  std::unordered_map<std::string, WidgetSettingValue>
  seedEntrySettings(const PluginEntry& entry, const std::unordered_map<std::string, WidgetSettingValue>& overrides) {
    std::unordered_map<std::string, WidgetSettingValue> seeded;
    seeded.reserve(entry.settings.size());
    for (const ManifestField& field : entry.settings) {
      if (const auto it = overrides.find(field.key); it != overrides.end()) {
        seeded.emplace(field.key, it->second);
      } else {
        seeded.emplace(field.key, field.defaultValue());
      }
    }
    return seeded;
  }

  std::optional<PluginManifest> parsePluginManifest(const std::filesystem::path& manifestPath, std::string* error) {
    const auto fail = [error](std::string message) -> std::optional<PluginManifest> {
      if (error != nullptr) {
        *error = std::move(message);
      }
      return std::nullopt;
    };

    toml::table root;
    try {
      root = toml::parse_file(manifestPath.string());
    } catch (const toml::parse_error& e) {
      return fail(std::string("parse error: ") + e.description().data());
    }

    PluginManifest manifest;
    manifest.id = tableString(root, "id");
    if (manifest.id.empty()) {
      return fail("missing mandatory key 'id'");
    }
    if (!isValidPluginId(manifest.id)) {
      return fail("invalid plugin id '" + manifest.id + "' (expected author/plugin)");
    }
    manifest.name = tableString(root, "name");
    if (manifest.name.empty()) {
      return fail("missing mandatory key 'name'");
    }
    manifest.minNoctalia = tableString(root, "min_noctalia");
    if (manifest.minNoctalia.empty()) {
      return fail("missing mandatory key 'min_noctalia'");
    }

    manifest.version = tableString(root, "version");
    manifest.author = tableString(root, "author");
    manifest.license = tableString(root, "license", "MIT");
    manifest.deprecated = tableBool(root, "deprecated", false);
    manifest.icon = tableString(root, "icon");
    manifest.description = tableString(root, "description");
    manifest.tags = tableStringArray(root, "tags");
    manifest.dependencies = tableStringArray(root, "dependencies");

    std::string manifestError;
    for (const auto& [kind, tableName] : kEntryKinds) {
      if (!parseEntries(root, kind, tableName, manifest, manifestError)) {
        return fail(manifestError);
      }
    }

    if (const auto* settings = root["setting"].as_array()) {
      for (const auto& node : *settings) {
        if (const auto* settingTable = node.as_table()) {
          auto field = parseField(*settingTable, manifestError);
          if (!field.has_value()) {
            return fail(manifestError);
          }
          if (!field->key.empty()) {
            manifest.settings.push_back(std::move(*field));
          }
        }
      }
    }

    std::unordered_set<std::string> seenEntryIds;
    for (const auto& entry : manifest.entries) {
      if (!seenEntryIds.insert(entry.id).second) {
        return fail("duplicate entry id '" + entry.id + "'");
      }
    }

    std::unordered_set<std::string> pluginLevelKeys;
    for (const auto& field : manifest.settings) {
      pluginLevelKeys.insert(field.key);
    }
    for (const auto& entry : manifest.entries) {
      for (const auto& field : entry.settings) {
        if (pluginLevelKeys.contains(field.key)) {
          kLog.warn(
              "plugin '{}' entry '{}' setting '{}' shadows a plugin-level setting; entry value wins", manifest.id,
              entry.id, field.key
          );
        }
      }
    }

    return manifest;
  }

  void mergePluginSettings(
      const PluginManifest& manifest, const std::unordered_map<std::string, WidgetSettingValue>& pluginOverrides,
      std::unordered_map<std::string, WidgetSettingValue>& seeded
  ) {
    for (const ManifestField& field : manifest.settings) {
      if (seeded.contains(field.key)) {
        continue; // entry-level setting declared the same key — entry wins
      }
      const auto it = pluginOverrides.find(field.key);
      seeded.emplace(field.key, it != pluginOverrides.end() ? it->second : field.defaultValue());
    }
  }

  bool settingsEqual(
      const std::unordered_map<std::string, WidgetSettingValue>& a,
      const std::unordered_map<std::string, WidgetSettingValue>& b
  ) {
    if (a.size() != b.size()) {
      return false;
    }
    for (const auto& [key, value] : a) {
      const auto it = b.find(key);
      if (it == b.end() || !valueEqual(value, it->second)) {
        return false;
      }
    }
    return true;
  }

} // namespace scripting
