#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_schema_fields.h"
#include "config/schema/diagnostics.h"
#include "config/schema/engine.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noctalia::config::schema {

  namespace {
    using TemplateColor = ThemeConfig::TemplateColorConfig;
    using UserTemplate = ThemeConfig::UserTemplateConfig;
    using CompareColor = ThemeConfig::TemplateCompareColorConfig;

    // [theme.templates.custom_colors]: a name-keyed map whose value is either a
    // bare color string or a { color, color_dark, color_light, blend } table.
    // Kept only when name+color are non-empty (color falls back to color_dark then
    // color_light when not provided); emitted only when the list is non-empty.
    // Entries and table keys of the wrong type are skipped with a warning under `path`.
    void parseCustomColorsMapImpl(
        const toml::table& map, std::vector<TemplateColor>& out, std::string_view path, Diagnostics& diag
    ) {
      for (const auto& [name, value] : map) {
        TemplateColor color;
        color.name = std::string(name.str());
        if (const auto* str = value.as_string()) {
          color.color = str->get();
        } else if (const auto* t = value.as_table()) {
          const std::string entryPath = joinPath(path, name.str());
          if (const auto* c = t->get_as<std::string>("color_dark")) {
            color.color_dark = c->get();
          } else {
            warnWrongType(*t, "color_dark", entryPath, diag, "a string");
          }
          if (const auto* c = t->get_as<std::string>("color_light")) {
            color.color_light = c->get();
          } else {
            warnWrongType(*t, "color_light", entryPath, diag, "a string");
          }
          if (const auto* c = t->get_as<std::string>("color")) {
            color.color = c->get();
          } else {
            warnWrongType(*t, "color", entryPath, diag, "a string");
            color.color = !color.color_dark.empty() ? color.color_dark : color.color_light;
          }
          if (const auto* b = t->get_as<bool>("blend")) {
            color.blend = b->get();
          } else {
            warnWrongType(*t, "blend", entryPath, diag, "a boolean");
          }
        } else {
          warnWrongType(diag, joinPath(path, name.str()), value, "a string or a table");
        }
        if (!StringUtils::trim(color.name).empty() && !StringUtils::trim(color.color).empty()) {
          out.push_back(std::move(color));
        }
      }
    }

    Field<ThemeConfig::TemplatesConfig> customColorsField() {
      return custom<ThemeConfig::TemplatesConfig>(
          "custom_colors",
          [](const toml::table& tbl, ThemeConfig::TemplatesConfig& out, std::string_view parentPath,
             Diagnostics& diag) {
            const auto* map = tbl["custom_colors"].as_table();
            if (map == nullptr) {
              warnWrongType(tbl, "custom_colors", parentPath, diag, "a table");
              return;
            }
            out.customColors.clear();
            parseCustomColorsMapImpl(*map, out.customColors, joinPath(parentPath, "custom_colors"), diag);
          },
          [](toml::table& tbl, const ThemeConfig::TemplatesConfig& in) {
            if (in.customColors.empty()) {
              return;
            }
            toml::table map;
            for (const auto& color : in.customColors) {
              toml::table colorTable;
              colorTable.insert_or_assign("color", color.color);
              colorTable.insert_or_assign("blend", color.blend);
              if (!color.color_dark.empty()) {
                colorTable.insert_or_assign("color_dark", color.color_dark);
              }
              if (!color.color_light.empty()) {
                colorTable.insert_or_assign("color_light", color.color_light);
              }
              map.insert_or_assign(color.name, std::move(colorTable));
            }
            tbl.insert_or_assign("custom_colors", std::move(map));
          }
      );
    }

    const Schema<UserTemplate>& userTemplateSchema() {
      static const Schema<UserTemplate> s = {
          field(&UserTemplate::enabled, "enabled"),
          field(&UserTemplate::inputPath, "input_path"),
          // input_path_modes is set only when both dark and light are present.
          custom<UserTemplate>(
              "input_path_modes",
              [](const toml::table& tbl, UserTemplate& out, std::string_view parentPath, Diagnostics& diag) {
                const auto* m = tbl["input_path_modes"].as_table();
                if (m == nullptr) {
                  warnWrongType(tbl, "input_path_modes", parentPath, diag, "a table");
                  return;
                }
                const auto* dark = m->get_as<std::string>("dark");
                if (dark == nullptr) {
                  warnWrongType(*m, "dark", joinPath(parentPath, "input_path_modes"), diag, "a string");
                }
                const auto* light = m->get_as<std::string>("light");
                if (light == nullptr) {
                  warnWrongType(*m, "light", joinPath(parentPath, "input_path_modes"), diag, "a string");
                }
                if (dark != nullptr && light != nullptr) {
                  out.inputPathModes = ThemeConfig::TemplateInputPathModesConfig{dark->get(), light->get()};
                }
              },
              [](toml::table& tbl, const UserTemplate& in) {
                if (in.inputPathModes.has_value()) {
                  toml::table modes;
                  modes.insert_or_assign("dark", in.inputPathModes->dark);
                  modes.insert_or_assign("light", in.inputPathModes->light);
                  tbl.insert_or_assign("input_path_modes", std::move(modes));
                }
              }
          ),
          // output_path accepts a single string or an array; always emitted as an array.
          custom<UserTemplate>(
              "output_path",
              [](const toml::table& tbl, UserTemplate& out, std::string_view parentPath, Diagnostics& diag) {
                const auto* node = tbl.get("output_path");
                if (node == nullptr) {
                  return;
                }
                out.outputPaths.clear();
                if (const auto* str = node->as_string()) {
                  out.outputPaths.push_back(str->get());
                } else if (const auto* arr = node->as_array()) {
                  out.outputPaths = readStringArray(*arr, joinPath(parentPath, "output_path"), diag);
                } else {
                  warnWrongType(diag, joinPath(parentPath, "output_path"), *node, "a string or an array of strings");
                }
              },
              [](toml::table& tbl, const UserTemplate& in) {
                toml::array arr;
                for (const auto& p : in.outputPaths) {
                  arr.push_back(p);
                }
                tbl.insert_or_assign("output_path", std::move(arr));
              }
          ),
          field(&UserTemplate::outputPathDynamic, "output_path_dynamic"),
          field(&UserTemplate::compareTo, "compare_to"),
          // colors_to_compare: array of { name, color }; emitted only when non-empty.
          custom<UserTemplate>(
              "colors_to_compare",
              [](const toml::table& tbl, UserTemplate& out, std::string_view parentPath, Diagnostics& diag) {
                const auto* arr = tbl["colors_to_compare"].as_array();
                if (arr == nullptr) {
                  warnWrongType(tbl, "colors_to_compare", parentPath, diag, "an array of tables");
                  return;
                }
                const auto itemPath = [&](std::size_t index) {
                  return joinPath(parentPath, "colors_to_compare") + '[' + std::to_string(index) + ']';
                };
                out.colorsToCompare.clear();
                for (std::size_t index = 0; index < arr->size(); ++index) {
                  const auto& item = (*arr)[index];
                  const auto* t = item.as_table();
                  if (t == nullptr) {
                    warnWrongType(diag, itemPath(index), item, "a table");
                    continue;
                  }
                  const auto* name = t->get_as<std::string>("name");
                  if (name == nullptr) {
                    warnWrongType(*t, "name", itemPath(index), diag, "a string");
                  }
                  const auto* color = t->get_as<std::string>("color");
                  if (color == nullptr) {
                    warnWrongType(*t, "color", itemPath(index), diag, "a string");
                  }
                  if (name != nullptr && color != nullptr) {
                    out.colorsToCompare.push_back(CompareColor{name->get(), color->get()});
                  }
                }
              },
              [](toml::table& tbl, const UserTemplate& in) {
                if (in.colorsToCompare.empty()) {
                  return;
                }
                toml::array arr;
                for (const auto& color : in.colorsToCompare) {
                  toml::table colorTable;
                  colorTable.insert_or_assign("name", color.name);
                  colorTable.insert_or_assign("color", color.color);
                  arr.push_back(std::move(colorTable));
                }
                tbl.insert_or_assign("colors_to_compare", std::move(arr));
              }
          ),
          field(&UserTemplate::preHook, "pre_hook"),
          field(&UserTemplate::postHook, "post_hook"),
          field(&UserTemplate::postAction, "post_action"),
          field(&UserTemplate::index, "index"),
          field(&UserTemplate::hookAsync, "hook_async"),
      };
      return s;
    }

    const Schema<ThemeConfig::TemplatesConfig>& templatesSchema() {
      static const Schema<ThemeConfig::TemplatesConfig> s = {
          field(&ThemeConfig::TemplatesConfig::enableBuiltinTemplates, "enable_builtin_templates"),
          field(&ThemeConfig::TemplatesConfig::builtinIds, "builtin_ids"),
          field(&ThemeConfig::TemplatesConfig::enableCommunityTemplates, "enable_community_templates"),
          field(&ThemeConfig::TemplatesConfig::communityIds, "community_ids"),
          customColorsField(),
          namedMap<ThemeConfig::TemplatesConfig, UserTemplate>(
              &ThemeConfig::TemplatesConfig::userTemplates, "user", userTemplateSchema(),
              [](UserTemplate& t, std::string_view name) { t.id = std::string(name); },
              [](const UserTemplate& t) { return t.id; }, /*readSkipEmptyName=*/true
          ),
      };
      return s;
    }
  } // namespace

  void parseCustomColorsMap(
      const toml::table& map, std::vector<ThemeConfig::TemplateColorConfig>& out, std::string_view path,
      Diagnostics& diag
  ) {
    parseCustomColorsMapImpl(map, out, path, diag);
  }

  void appendUniqueCustomColors(
      std::vector<ThemeConfig::TemplateColorConfig>& into, std::vector<ThemeConfig::TemplateColorConfig> additional
  ) {
    for (auto& color : additional) {
      const bool exists = std::ranges::any_of(into, [&](const auto& existing) { return existing.name == color.name; });
      if (!exists) {
        into.push_back(std::move(color));
      }
    }
  }

  void liftTemplateConfigCustomColors(const toml::table& root, Config& config, Diagnostics& diag) {
    if (const auto* configTbl = root["config"].as_table()) {
      if (const auto* customColors = (*configTbl)["custom_colors"].as_table()) {
        std::vector<ThemeConfig::TemplateColorConfig> lifted;
        parseCustomColorsMap(*customColors, lifted, "config.custom_colors", diag);
        appendUniqueCustomColors(config.theme.templates.customColors, std::move(lifted));
      } else {
        warnWrongType(*configTbl, "custom_colors", "config", diag, "a table");
      }
    }
  }

  const Schema<ThemeConfig>& themeSchema() {
    static const Schema<ThemeConfig> s = {
        enumField(&ThemeConfig::source, "source", kPaletteSources),
        field(&ThemeConfig::builtinPalette, "builtin"),
        field(&ThemeConfig::communityPalette, "community_palette"),
        field(&ThemeConfig::customPalette, "custom_palette"),
        field(&ThemeConfig::wallpaperScheme, "wallpaper_scheme"),
        enumField(&ThemeConfig::mode, "mode", kThemeModes),
        enumField(&ThemeConfig::shellMode, "shell_mode", kShellThemeModes),
        field(&ThemeConfig::pureBlackDark, "pure_black_dark"),
        enumField(&ThemeConfig::terminalColors, "terminal_colors", kTerminalColorsOptions),
        field(&ThemeConfig::syncColorScheme, "sync_color_scheme"),
        subTable(&ThemeConfig::templates, "templates", templatesSchema()),
    };
    return s;
  }

} // namespace noctalia::config::schema
