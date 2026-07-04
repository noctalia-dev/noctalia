#pragma once

#include "config/config_types.h"
#include "scripting/plugin_manager.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class Flex;

namespace scripting {
  struct PluginManifest;
}

namespace settings {

  class SettingsControlFactory;

  // Data + actions for the Plugins settings section. Populated by SettingsWindow
  // from the PluginManager; the section is fully custom (no registry entries).
  struct SettingsPluginsContext {
    float scale = 1.0f;
    std::string_view selectedSection;
    std::vector<scripting::PluginStatus> plugins;
    std::vector<PluginSourceConfig> sources;
    bool pluginsLoading = false;

    std::function<void(std::string id, bool enable)> setEnabled;
    // True while a git-source plugin's runtime export runs in the background; the row
    // shows a spinner in place of the toggle until it lands.
    std::function<bool(const std::string& id)> isEnabling;
    std::function<void()> addSource;
    std::function<void(PluginSourceConfig source, bool enabled)> setSourceEnabled;
    std::function<void(PluginSourceConfig source)> editSource;
    std::function<void(std::string source)> updateSource;
    std::function<void()> refresh;

    // Used to derive current toggle state while async discovery refreshes.
    const Config* config = nullptr;
    std::function<void(std::string id)> onConfigure;
    std::function<void(std::string id)> onRemove;
    std::function<void()> openStore;

    // Plugin id awaiting delete confirmation; its row shows an inline confirm panel.
    std::string pendingDeletePluginId;
    std::function<void(std::string id)> requestDeleteConfirm;
    std::function<void()> cancelDelete;
  };

  // Render the Plugins section into `content` when ctx.selectedSection == "plugins".
  void addSettingsPlugins(Flex& content, SettingsPluginsContext ctx);

  void buildPluginSettingsEditor(
      Flex& body, const Config& cfg, SettingsControlFactory& factory, const std::string& pluginId,
      const scripting::PluginManifest& manifest, bool showAdvanced, float scale
  );

} // namespace settings
