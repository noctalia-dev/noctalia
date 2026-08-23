#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "scripting/plugin_script_watcher.h"
#include "scripting/script_runtime.h"
#include "shell/control_center/shortcut_registry.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

class HttpClient;
class ClipboardService;
class CompositorPlatform;
namespace scripting {
  struct PluginRuntimeContext;
  class ScriptApiContext;
} // namespace scripting

// A control-center shortcut backed by a plugin's [[shortcut]] entry. The native
// Shortcut interface is polled, so the latest label/icon/active/enabled patch is
// cached and a redraw is kicked when it changes. Instances are reused across
// Control Center open/close (HomeTab keeps them); timers and the source file
// watch are suspended while the panel is closed. Reopen reloads the script if
// the source file changed while closed.
class PluginShortcut : public Shortcut {
public:
  explicit PluginShortcut(scripting::PluginRuntimeContext context);
  ~PluginShortcut() override;

  [[nodiscard]] std::string_view id() const override { return m_entryId; }
  [[nodiscard]] std::string defaultLabel() const override { return m_label.empty() ? m_entryId : m_label; }
  [[nodiscard]] std::string displayLabel() const override { return m_label.empty() ? m_entryId : m_label; }
  [[nodiscard]] std::string_view iconOn() const override { return m_iconOn; }
  [[nodiscard]] std::string_view iconOff() const override { return m_iconOff; }
  [[nodiscard]] bool isToggle() const override { return true; }
  [[nodiscard]] bool enabled() const override { return m_enabled; }
  [[nodiscard]] bool active() const override { return m_active; }
  void onClick() override;
  void onRightClick() override;
  void onPanelClose() override;
  void onPanelOpen() override;

private:
  void start();
  void setupScriptWatch();
  void teardownScriptWatch();
  void reloadScript(bool notifyUser = true);
  void recordLoadedSourceMtime();
  void recordLoadedModuleMtimes(std::span<const std::filesystem::path> paths);
  [[nodiscard]] bool sourceChangedSinceLoad() const;
  void resetPresentation();
  void handleResult(const scripting::ScriptResult& result);
  void armTimer();
  [[nodiscard]] scripting::ScriptSnapshot makeScriptSnapshot() const;
  [[nodiscard]] std::string focusedOutputName() const;

  std::string m_entryId;
  std::filesystem::path m_sourcePath;
  std::filesystem::path m_pluginDir;
  std::unordered_map<std::string, WidgetSettingValue> m_settings;
  scripting::ScriptApiContext& m_scriptApi;
  FileWatcher* m_fileWatcher = nullptr;
  HttpClient* m_httpClient = nullptr;
  ClipboardService* m_clipboard = nullptr;
  CompositorPlatform* m_platform = nullptr;
  std::shared_ptr<scripting::ScriptRuntime> m_runtime;
  scripting::ScriptRuntime::SubscriberId m_subscription = 0;
  scripting::PluginScriptWatcher m_scriptWatcher;
  std::string m_label;
  std::string m_iconOn = "circle";
  std::string m_iconOff = "circle";
  bool m_active = false;
  bool m_enabled = true;
  Timer m_updateTimer;
  int m_updateIntervalMs = 1000;
  std::filesystem::file_time_type m_loadedSourceMtime;
  std::unordered_map<std::filesystem::path, std::filesystem::file_time_type> m_loadedModuleMtimes;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
