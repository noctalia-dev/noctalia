#pragma once

#include "scripting/plugin_layer_config.h"
#include "scripting/script_runtime_types.h"
#include "shell/plugin_layer/plugin_layer.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ClipboardService;
class FileWatcher;
class HttpClient;
struct KeyboardEvent;
struct PointerEvent;

namespace scripting {
  class ScriptApiContext;
} // namespace scripting

// Hosts plugin `[[layer]]` entries. A layer is lazy: nothing runs until noctalia.layer.show()
// (or `visible = true` in the manifest) asks for it, which creates one PluginLayer per matching
// output; hide destroys them. Surface properties start from the manifest and follow
// noctalia.layer.configure() until the plugin reloads.
class PluginLayerHost {
public:
  struct Definition {
    std::string entryId; // "author/plugin:entry"
    std::filesystem::path sourcePath;
    std::filesystem::path pluginDir;
    scripting::ScriptSettings settings;
    scripting::PluginLayerConfig config;
    bool visible = false;
  };

  PluginLayerHost(
      PluginLayerServices services, scripting::ScriptApiContext& scriptApi, FileWatcher* fileWatcher,
      HttpClient* httpClient, ClipboardService* clipboard
  );
  ~PluginLayerHost();

  PluginLayerHost(const PluginLayerHost&) = delete;
  PluginLayerHost& operator=(const PluginLayerHost&) = delete;

  // Replaces the registered entries. An entry whose definition is unchanged keeps its state and
  // instances; a new or changed one starts from its manifest defaults.
  void reload(std::vector<Definition> definitions);
  void command(
      scripting::PluginLayerCommand command, const std::string& entryId, const scripting::PluginLayerConfigPatch& patch
  );
  void onOutputChange();
  void onConfigReload();
  bool onPointerEvent(const PointerEvent& event);
  bool onKeyboardEvent(const KeyboardEvent& event);

private:
  struct Entry {
    Definition definition;
    scripting::PluginLayerConfig config;
    bool shown = false;
    // Output picked for output = "focused" when the layer was shown.
    wl_output* focusedOutput = nullptr;
    std::vector<std::unique_ptr<PluginLayer>> instances;
  };

  [[nodiscard]] Entry* findEntry(const std::string& entryId);
  void show(Entry& entry);
  void hide(Entry& entry);
  // Creates and destroys instances so the shown entry covers exactly its target outputs.
  void syncInstances(Entry& entry);
  [[nodiscard]] bool targetsOutput(const Entry& entry, const WaylandOutput& output) const;
  [[nodiscard]] PluginLayer* findInstance(wl_surface* surface);

  PluginLayerServices m_services;
  scripting::ScriptApiContext& m_scriptApi;
  FileWatcher* m_fileWatcher = nullptr;
  HttpClient* m_httpClient = nullptr;
  ClipboardService* m_clipboard = nullptr;
  std::vector<std::unique_ptr<Entry>> m_entries;
};
