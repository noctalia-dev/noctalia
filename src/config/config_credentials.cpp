#include "config/config_credentials.h"

#include "config/schema/config_schema.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace noctalia::config {

  namespace {

    using KeySet = std::unordered_set<std::string>;

    void addSecretKeys(const std::vector<scripting::ManifestField>& fields, KeySet& out) {
      for (const auto& field : fields) {
        if (field.secret) {
          out.insert(field.key);
        }
      }
    }

    void applyToKey(toml::table& table, std::string_view key, CredentialExportMode mode) {
      toml::node* node = table.get(key);
      if (node == nullptr) {
        return;
      }
      if (mode == CredentialExportMode::Omit) {
        table.erase(key);
        return;
      }
      if (node->is_array()) {
        table.insert_or_assign(key, toml::array{});
      } else if (node->is_table()) {
        table.insert_or_assign(key, toml::table{});
      } else {
        table.insert_or_assign(key, "");
      }
    }

    void applyToKeys(toml::table& table, const KeySet& keys, CredentialExportMode mode) {
      for (const auto& key : keys) {
        applyToKey(table, key, mode);
      }
    }

    // The secret entry-level keys of a plugin widget type ("author/plugin:entry"), or none.
    KeySet widgetSecretKeys(
        const scripting::PluginRegistry& registry, std::string_view type, scripting::PluginEntryKind kind
    ) {
      KeySet keys;
      const auto resolved = registry.resolve(type);
      if (resolved.has_value() && resolved->entry->kind == kind) {
        addSecretKeys(resolved->entry->settings, keys);
      }
      return keys;
    }

    // [plugin_settings."author/plugin"] holds plugin-level settings plus the settings of the
    // plugin's singleton panels.
    void
    applyToPluginSettings(toml::table& root, const scripting::PluginRegistry& registry, CredentialExportMode mode) {
      auto* pluginSettings = root["plugin_settings"].as_table();
      if (pluginSettings == nullptr) {
        return;
      }
      for (auto& [pluginId, node] : *pluginSettings) {
        auto* perPlugin = node.as_table();
        const scripting::PluginManifest* manifest = registry.findManifest(pluginId.str());
        if (perPlugin == nullptr || manifest == nullptr) {
          continue;
        }
        KeySet keys;
        addSecretKeys(manifest->settings, keys);
        for (const auto& entry : manifest->entries) {
          if (entry.kind == scripting::PluginEntryKind::Panel) {
            addSecretKeys(entry.settings, keys);
          }
        }
        applyToKeys(*perPlugin, keys, mode);
      }
    }

    // [widget.<name>] keeps entry settings inline next to `type`, which defaults to the name.
    void applyToBarWidgets(toml::table& root, const scripting::PluginRegistry& registry, CredentialExportMode mode) {
      auto* widgets = root["widget"].as_table();
      if (widgets == nullptr) {
        return;
      }
      for (auto& [name, node] : *widgets) {
        auto* widget = node.as_table();
        if (widget == nullptr) {
          continue;
        }
        const std::string type = (*widget)["type"].value<std::string>().value_or(std::string(name.str()));
        applyToKeys(*widget, widgetSecretKeys(registry, type, scripting::PluginEntryKind::Widget), mode);
      }
    }

    // [desktop_widgets.widget.<id>] and [lockscreen_widgets.widget.<id>] nest entry settings
    // under `settings`.
    void applyToPlacedWidgets(
        toml::table& root, std::string_view section, const scripting::PluginRegistry& registry,
        CredentialExportMode mode
    ) {
      auto* widgets = root[section]["widget"].as_table();
      if (widgets == nullptr) {
        return;
      }
      for (auto& [id, node] : *widgets) {
        auto* widget = node.as_table();
        auto* settings = widget != nullptr ? (*widget)["settings"].as_table() : nullptr;
        if (settings == nullptr) {
          continue;
        }
        const std::string type = (*widget)["type"].value<std::string>().value_or(std::string{});
        applyToKeys(*settings, widgetSecretKeys(registry, type, scripting::PluginEntryKind::DesktopWidget), mode);
      }
    }

    // ICS and CalDAV server URLs often carry an access token or user:password.
    void applyToCalendarAccounts(toml::table& root, CredentialExportMode mode) {
      auto* accounts = root["calendar"]["account"].as_table();
      if (accounts == nullptr) {
        return;
      }
      for (auto& [id, node] : *accounts) {
        if (auto* account = node.as_table()) {
          applyToKey(*account, "server_url", mode);
        }
      }
    }

    // Git source URLs may embed user:token credentials. Sources default to the git kind.
    void applyToPluginSources(toml::table& root, CredentialExportMode mode) {
      auto* sources = root["plugins"]["source"].as_array();
      if (sources == nullptr) {
        return;
      }
      for (auto& node : *sources) {
        auto* source = node.as_table();
        if (source != nullptr && (*source)["kind"].value<std::string>().value_or("git") == "git") {
          applyToKey(*source, "location", mode);
        }
      }
    }

  } // namespace

  std::optional<CredentialExportMode> credentialExportModeFromString(std::string_view name) {
    if (name == "keep") {
      return CredentialExportMode::Keep;
    }
    if (name == "omit") {
      return CredentialExportMode::Omit;
    }
    if (name == "blank") {
      return CredentialExportMode::Blank;
    }
    return std::nullopt;
  }

  void applyCredentialExportMode(toml::table& root, CredentialExportMode mode) {
    if (mode == CredentialExportMode::Keep) {
      return;
    }
    // Resolve manifests before redaction: the plugin sources themselves are redacted below.
    scripting::PluginRegistry registry;
    scripting::applyPluginSourcesToRegistry(registry, schema::readPluginsSection(root));

    applyToPluginSettings(root, registry, mode);
    applyToBarWidgets(root, registry, mode);
    applyToPlacedWidgets(root, "desktop_widgets", registry, mode);
    applyToPlacedWidgets(root, "lockscreen_widgets", registry, mode);
    applyToCalendarAccounts(root, mode);
    applyToPluginSources(root, mode);
  }

} // namespace noctalia::config
