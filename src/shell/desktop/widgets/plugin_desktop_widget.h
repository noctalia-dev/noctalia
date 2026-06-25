#pragma once

#include "config/config_types.h"
#include "core/file_watcher.h"
#include "core/timer_manager.h"
#include "scripting/plugin_ipc.h"
#include "scripting/script_runtime.h"
#include "shell/desktop/desktop_widget.h"
#include "ui/ui_tree.h"
#include "ui/ui_tree_reconciler.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class ClipboardService;
class Flex;
class HttpClient;
namespace scripting {
  struct PluginRuntimeContext;
  class ScriptApiContext;
} // namespace scripting

// A desktop widget backed by a plugin's `[[desktop_widget]]` entry. The script
// runs off-thread on its own Luau runtime and describes its UI declaratively:
// `desktopWidget.render(ui.column{...})` produces a UiTreeNode tree that the
// reconciler maps onto retained src/ui/controls. Positional fields (cx/cy, box,
// rotation) stay host-owned; the script only reads its declared settings.
class PluginDesktopWidget : public DesktopWidget, public scripting::PluginIpcEndpoint {
public:
  PluginDesktopWidget(scripting::PluginRuntimeContext context, std::string outputName);
  ~PluginDesktopWidget() override;

  void create() override;

  [[nodiscard]] bool wantsSecondTicks() const override { return m_wantsSecondTicks; }
  [[nodiscard]] bool needsFrameTick() const override { return m_needsFrameTick; }
  void onFrameTick(float deltaMs, Renderer& renderer) override;

  // PluginIpcEndpoint
  [[nodiscard]] std::string_view ipcEntryId() const override { return m_entryId; }
  [[nodiscard]] std::string_view ipcOutputName() const override { return m_outputName; }
  [[nodiscard]] std::string_view ipcBarName() const override { return {}; }
  [[nodiscard]] DispatchResult
  dispatchIpc(std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;

  void handleScriptResult(scripting::ScriptResult result);
  [[nodiscard]] scripting::ScriptSnapshot makeScriptSnapshot() const;
  [[nodiscard]] std::string resolvePluginPath(const std::string& path) const;
  void startUpdateTimer();
  void setupScriptWatch();
  void teardownScriptWatch();
  void reloadScript();

  std::string m_entryId; // "author/plugin:entry"
  std::filesystem::path m_sourcePath;
  std::filesystem::path m_pluginDir;
  std::string m_outputName;
  scripting::ScriptApiContext& m_scriptApi;
  std::unordered_map<std::string, WidgetSettingValue> m_settings;
  std::shared_ptr<scripting::ScriptRuntime> m_runtime;
  scripting::ScriptRuntime::SubscriberId m_runtimeSubscription = 0;
  FileWatcher* m_fileWatcher = nullptr;
  HttpClient* m_httpClient = nullptr;
  ClipboardService* m_clipboard = nullptr;
  FileWatcher::WatchId m_watchId = 0;
  Timer m_updateTimer;

  Flex* m_flex = nullptr;
  ui::UiTreeReconciler m_reconciler;
  std::optional<ui::UiTreeNode> m_tree;
  bool m_treeDirty = false;
  int m_updateIntervalMs = 1000;
  bool m_wantsSecondTicks = false;
  bool m_needsFrameTick = false;
  bool m_hasOnIpc = false;
  bool m_hasOnIpcKnown = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
