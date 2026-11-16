#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_schema_fields.h"
#include "config/schema/diagnostics.h"
#include "config/schema/engine.h"
#include "scripting/plugin_id.h"

#include <string>
#include <string_view>
#include <unordered_set>

namespace noctalia::config::schema {

  namespace {
    const Schema<PluginSourceConfig>& pluginSourceSchema() {
      static const Schema<PluginSourceConfig> s = {
          field(&PluginSourceConfig::name, "name"),
          enumField(&PluginSourceConfig::kind, "kind", kPluginSourceKinds),
          field(&PluginSourceConfig::location, "location"),
          field(&PluginSourceConfig::enabled, "enabled"),
          finalize<PluginSourceConfig>([](PluginSourceConfig& src, std::string_view parentPath, Diagnostics& diag) {
            if (!src.name.empty() && !isValidPluginSourceName(src.name)) {
              diag.warn(
                  joinPath(parentPath, "name"),
                  "invalid plugin source name; use letters, digits, '.', '_' or '-', starting with a letter or digit"
              );
            }
          }),
      };
      return s;
    }
  } // namespace

  const Schema<PluginsConfig>& pluginsSchema() {
    static const Schema<PluginsConfig> s = {
        arrayOf<PluginsConfig, PluginSourceConfig>(
            &PluginsConfig::sources, "source", pluginSourceSchema(),
            [](const PluginSourceConfig& src) { return isValidPluginSourceName(src.name); }
        ),
        field(&PluginsConfig::enabled, "enabled"),
        // auto_update accepts only "all"|"official"|"none"
        custom<PluginsConfig>(
            "auto_update",
            [](const toml::table& tbl, PluginsConfig& out, std::string_view parentPath, Diagnostics& diag) {
              if (auto v = tbl["auto_update"].value<std::string>()) {
                const std::string trimmed = StringUtils::trim(*v);
                if (auto parsed = enumFromKey(kPluginAutoUpdateModes, trimmed)) {
                  out.autoUpdate = *parsed;
                } else {
                  diag.error(
                      joinPath(parentPath, "auto_update"), "unknown value \"" + *v + "\"; expected all|official|none"
                  );
                }
              } else if (tbl.contains("auto_update")) {
                diag.error(joinPath(parentPath, "auto_update"), "expected all|official|none");
              }
            },
            [](toml::table& tbl, const PluginsConfig& in) {
              tbl.insert_or_assign("auto_update", std::string(enumToKey(kPluginAutoUpdateModes, in.autoUpdate)));
            }
        ),
        finalize<PluginsConfig>([](PluginsConfig& plugins, std::string_view parentPath, Diagnostics& diag) {
          for (auto it = plugins.enabled.begin(); it != plugins.enabled.end();) {
            if (scripting::isValidPluginId(*it)) {
              ++it;
              continue;
            }
            diag.warn(joinPath(parentPath, "enabled"), "invalid plugin id \"" + *it + "\"; expected author/plugin");
            it = plugins.enabled.erase(it);
          }
          // Duplicate names would share one checkout on disk: keep the first, error on the rest.
          std::unordered_set<std::string> seen;
          for (auto it = plugins.sources.begin(); it != plugins.sources.end();) {
            if (seen.insert(it->name).second) {
              ++it;
              continue;
            }
            diag.error(
                joinPath(parentPath, "source"),
                "duplicate plugin source name \"" + it->name + "\"; source names must be unique"
            );
            it = plugins.sources.erase(it);
          }
        }),
    };
    return s;
  }

  PluginsConfig readPluginsSection(const toml::table& root) {
    PluginsConfig plugins;
    const auto* pluginsTbl = root["plugins"].as_table();
    if (pluginsTbl != nullptr) {
      Diagnostics sink;
      readInto(*pluginsTbl, plugins, pluginsSchema(), "plugins", sink);
    }
    const bool sourcesConfigured = pluginsTbl != nullptr && (*pluginsTbl)["source"].as_array() != nullptr;
    if (!sourcesConfigured && plugins.sources.empty()) {
      plugins.sources = defaultPluginSources();
    }
    return plugins;
  }

} // namespace noctalia::config::schema
