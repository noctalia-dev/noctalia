#pragma once

#include "core/files/file_watcher.h"
#include "core/timer_manager.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/script_runtime.h"
#include "shell/panel/panel.h"
#include "ui/ui_tree.h"
#include "ui/ui_tree_reconciler.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

class ClipboardService;
class Flex;
class HttpClient;
class Node;
namespace scripting {
  struct PluginRuntimeContext;
  class ScriptApiContext;
} // namespace scripting

// Static panel options parsed from the manifest `[[panel]]` entry. Size is
// host-owned and declared once so the surface is sized correctly on first open.
struct PluginPanelOptions {
  double width = 0.0;  // logical pixels; 0 = host default
  double height = 0.0; // logical pixels; 0 = host default
  // Fill the output's available extent on this axis; the numeric size is then
  // only the fallback if the compositor never assigns one.
  bool widthFill = false;
  bool heightFill = false;
  bool dismissOnOutsideClick = true;
  scripting::PluginPanelShellConfig shellConfig;
};

// A panel backed by a plugin's `[[panel]]` entry. Like PluginDesktopWidget it
// runs the script off-thread on its own Luau runtime and reconciles the tree
// from `panel.render(ui.column{...})` into retained src/ui/controls. The runtime
// starts lazily on first open (PanelManager calls create() then), so registered
// but never-opened plugin panels cost nothing.
class PluginPanel : public Panel, public scripting::PluginIpcEndpoint {
public:
  PluginPanel(scripting::PluginRuntimeContext context, PluginPanelOptions options);
  ~PluginPanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(m_preferredWidth); }
  [[nodiscard]] float preferredHeight() const override { return scaled(m_preferredHeight); }
  [[nodiscard]] bool fillsWidth() const noexcept override { return m_widthFill; }
  [[nodiscard]] bool fillsHeight() const noexcept override { return m_heightFill; }
  [[nodiscard]] bool dismissOnOutsideClick() const override { return m_dismissOnOutsideClick; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return m_shellConfig.placement; }
  [[nodiscard]] std::string panelScreenPosition() const override { return m_shellConfig.position; }
  [[nodiscard]] bool panelOpenNearClick() const override { return m_shellConfig.openNearClick; }
  [[nodiscard]] InputArea* takePendingFocusArea() override { return std::exchange(m_pendingFocusArea, nullptr); }

  // PluginIpcEndpoint
  [[nodiscard]] std::string_view ipcEntryId() const override { return m_entryId; }
  [[nodiscard]] std::string_view ipcOutputName() const override { return {}; }
  [[nodiscard]] std::string_view ipcBarName() const override { return {}; }
  [[nodiscard]] DispatchResult
  dispatchIpc(std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot) override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  void handleScriptResult(scripting::ScriptResult result);
  [[nodiscard]] scripting::ScriptSnapshot makeScriptSnapshot() const;
  [[nodiscard]] std::string resolvePluginPath(const std::string& path) const;
  void startScript();
  void startTickTimer();
  void setupScriptWatch();
  void teardownScriptWatch();
  void reloadScript();

  std::string m_entryId; // "author/plugin:entry"
  std::filesystem::path m_sourcePath;
  std::filesystem::path m_pluginDir;
  scripting::ScriptApiContext& m_scriptApi;
  std::unordered_map<std::string, WidgetSettingValue> m_settings;
  std::shared_ptr<scripting::ScriptRuntime> m_runtime;
  scripting::ScriptRuntime::SubscriberId m_runtimeSubscription = 0;
  FileWatcher* m_fileWatcher = nullptr;
  HttpClient* m_httpClient = nullptr;
  ClipboardService* m_clipboard = nullptr;
  FileWatcher::WatchId m_watchId = 0;
  Timer m_tickTimer;

  Flex* m_flex = nullptr;
  Flex* m_contentFlex = nullptr;
  Node* m_dragOverlay = nullptr;
  InputArea* m_pendingFocusArea = nullptr;
  ui::UiTreeReconciler m_reconciler;
  std::optional<ui::UiTreeNode> m_tree;
  bool m_treeDirty = false;
  bool m_wantsSecondTicks = false;
  bool m_open = false;
  bool m_hasOnIpc = false;
  bool m_hasOnIpcKnown = false;
  float m_preferredWidth;
  float m_preferredHeight;
  bool m_widthFill = false;
  bool m_heightFill = false;
  bool m_dismissOnOutsideClick = true;
  scripting::PluginPanelShellConfig m_shellConfig;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
