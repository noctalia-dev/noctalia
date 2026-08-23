#pragma once

#include "core/input/key_chord.h"
#include "core/timer_manager.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_script_watcher.h"
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
class ContextMenuPopup;
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
  // One of scripting::kPanelKeyboardFocusModes.
  std::string keyboardFocus = "on_demand";
  bool persistent = false;
  // Key chord specs the panel takes over while focused, verbatim from the manifest.
  std::vector<std::string> captureKeys;
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
  void onFrameTick(float deltaMs) override;

  [[nodiscard]] float preferredWidth() const override { return scaled(m_preferredWidth); }
  [[nodiscard]] float preferredHeight() const override { return scaled(m_preferredHeight); }
  [[nodiscard]] bool fillsWidth() const noexcept override { return m_widthFill; }
  [[nodiscard]] bool fillsHeight() const noexcept override { return m_heightFill; }
  [[nodiscard]] bool dismissOnOutsideClick() const override { return m_dismissOnOutsideClick; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return m_keyboardMode; }
  [[nodiscard]] bool isPersistent() const noexcept override { return m_persistent; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return m_shellConfig.placement; }
  [[nodiscard]] std::string panelScreenPosition() const override { return m_shellConfig.position; }
  [[nodiscard]] bool panelOpenNearClick() const override { return m_shellConfig.openNearClick; }
  [[nodiscard]] InputArea* takePendingFocusArea() override { return std::exchange(m_pendingFocusArea, nullptr); }
  [[nodiscard]] bool dismissTransientUi() override;

  // Delivers a manifest-declared capture_keys chord to the script's onKey(chord, pressed) and
  // reports it consumed. Declared chords only: everything else keeps its host behaviour, and a
  // focused text input still wins printable keys (PanelManager reserves those before calling).
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

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
  void openContextMenu(scripting::ScriptContextMenuRequest request);
  void closeContextMenu();
  [[nodiscard]] scripting::ScriptSnapshot makeScriptSnapshot() const;
  [[nodiscard]] std::string resolvePluginPath(const std::string& path) const;
  void releaseCapturedKeys();
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
  // Parsed capture_keys, paired with the verbatim spec the script is called back with.
  struct CaptureKey {
    KeyChord chord;
    std::string spec;
    // Physically down. Key repeat re-sends press events, which a hold-to-act interaction must
    // not see as new presses, so a repeat is consumed without a second onKey call.
    bool held = false;
  };
  std::vector<CaptureKey> m_captureKeys;
  std::shared_ptr<scripting::ScriptRuntime> m_runtime;
  scripting::ScriptRuntime::SubscriberId m_runtimeSubscription = 0;
  FileWatcher* m_fileWatcher = nullptr;
  HttpClient* m_httpClient = nullptr;
  ClipboardService* m_clipboard = nullptr;
  scripting::PluginScriptWatcher m_scriptWatcher;
  Timer m_tickTimer;

  Flex* m_flex = nullptr;
  Flex* m_contentFlex = nullptr;
  Node* m_dragOverlay = nullptr;
  InputArea* m_pendingFocusArea = nullptr;
  ui::UiTreeReconciler m_reconciler;
  std::unique_ptr<ContextMenuPopup> m_contextMenuPopup;
  std::optional<ui::UiTreeNode> m_tree;
  bool m_treeDirty = false;
  bool m_wantsSecondTicks = false;
  bool m_needsFrameTick = false;
  bool m_open = false;
  std::uint64_t m_openGeneration = 0;
  bool m_hasOnIpc = false;
  bool m_hasOnIpcKnown = false;
  float m_preferredWidth;
  float m_preferredHeight;
  bool m_widthFill = false;
  bool m_heightFill = false;
  bool m_dismissOnOutsideClick = true;
  LayerShellKeyboard m_keyboardMode = LayerShellKeyboard::OnDemand;
  bool m_persistent = false;
  scripting::PluginPanelShellConfig m_shellConfig;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
