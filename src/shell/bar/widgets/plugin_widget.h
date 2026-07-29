#pragma once

#include "config/config_types.h"
#include "core/files/file_watcher.h"
#include "core/timer_manager.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_runtime_context.h"
#include "scripting/script_runtime.h"
#include "shell/bar/widget.h"
#include "ui/palette.h"
#include "ui/ui_tree.h"
#include "ui/ui_tree_reconciler.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class Flex;
class Glyph;
class Image;
class InputArea;
class Label;
class CompositorPlatform;
class ClipboardService;
class HttpClient;
class PipeWireSpectrum;
class MprisService;
namespace scripting {
  class ScriptApiContext;
}

// A bar widget backed by a plugin's `[[widget]]` entry. Each instance owns its
// own Luau runtime (no shared scope). Settings are pre-seeded from the manifest
// defaults + the instance's configured values.
class PluginWidget : public Widget, public scripting::PluginIpcEndpoint {
public:
  PluginWidget(
      scripting::PluginRuntimeContext context, std::string barName, std::string outputName, bool enableScroll = true
  );
  ~PluginWidget() override;

  void create() override;

  void luaSetText(std::string_view text);
  void luaSetGlyph(std::string_view name);
  void luaSetImage(std::string_view path, bool watch, float width, float height);
  void luaSetTooltip(const scripting::ScriptTooltipPatch& tooltip);
  void luaSetFont(std::string_view family, std::string_view baseline);
  void luaSetColor(std::string_view role, std::string_view mode);
  void luaSetGlyphColor(std::string_view role, std::string_view mode);
  void luaSetVisible(bool visible);
  void luaSetUpdateInterval(float ms);
  void setUpdateDeferralCallback(std::function<bool()> callback);
  [[nodiscard]] bool isVertical() const { return m_isVertical; }

  // PluginIpcEndpoint
  [[nodiscard]] std::string_view ipcEntryId() const override { return m_entryId; }
  [[nodiscard]] std::string_view ipcOutputName() const override { return m_outputName; }
  [[nodiscard]] std::string_view ipcBarName() const override { return m_barName; }
  [[nodiscard]] DispatchResult
  dispatchIpc(std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot) override;

private:
  enum class ScriptColorMode {
    Auto,
    Script,
  };

  struct ScriptColorState {
    std::optional<ColorSpec> color;
    ScriptColorMode mode = ScriptColorMode::Auto;

    bool operator==(const ScriptColorState&) const = default;
  };

  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;

  [[nodiscard]] ColorSpec resolveScriptColor(const ScriptColorState& state) const noexcept;
  [[nodiscard]] static ScriptColorMode scriptColorModeFromToken(std::string_view token) noexcept;
  [[nodiscard]] static std::optional<ColorSpec> scriptColorFromToken(std::string_view token) noexcept;
  [[nodiscard]] std::filesystem::path resolvePluginPath(std::string_view path) const;

  void reloadScript();
  void reloadImage();
  void handleScriptResult(scripting::ScriptResult result);
  void applyScriptPatch(const scripting::ScriptPatch& patch);
  void applyUiTreePatch(const ui::UiTreeNode& patchTree);
  [[nodiscard]] scripting::ScriptSnapshot makeScriptSnapshot() const;
  [[nodiscard]] std::string focusedOutputName() const;
  void syncImage(Renderer& renderer);
  void setupImageWatch();
  void teardownImageWatch();
  void scheduleImageReloadRetry();
  void setupScriptWatch();
  void teardownScriptWatch();
  void startUpdateTimer();
  void armDeferredUpdate(std::uint64_t generation);
  void handleUpdateTimer();
  [[nodiscard]] std::chrono::milliseconds initialUpdateDelay(std::chrono::milliseconds interval) const noexcept;
  void runScriptUpdate();
  void scheduleDeferredUpdate();
  [[nodiscard]] bool shouldDeferUpdate() const;

  // Audio-reactive widgets (e.g. bongocat): subscribe to the PipeWire spectrum and
  // forward each frame to the script's onAudioSpectrum(values, state) callback.
  void setupAudioSpectrum();
  void teardownAudioSpectrum();
  void handleAudioSpectrumChanged();

  std::string m_entryId; // "author/plugin:entry"
  std::filesystem::path m_sourcePath;
  std::filesystem::path m_pluginDir;
  std::string m_barName;
  std::string m_outputName;
  scripting::ScriptApiContext& m_scriptApi;
  std::filesystem::path m_resolvedImagePath;
  std::unordered_map<std::string, WidgetSettingValue> m_settings;
  std::shared_ptr<scripting::ScriptRuntime> m_runtime;
  scripting::ScriptRuntime::SubscriberId m_runtimeSubscription = 0;
  FileWatcher* m_fileWatcher = nullptr;
  CompositorPlatform* m_platform = nullptr;
  ClipboardService* m_clipboard = nullptr;
  HttpClient* m_httpClient = nullptr;
  PipeWireSpectrum* m_audioSpectrum = nullptr;
  MprisService* m_mpris = nullptr;
  std::uint64_t m_audioSpectrumListenerId = 0;
  int m_audioSpectrumBands = 16;
  bool m_audioSpectrumEnabled = false;
  FileWatcher::WatchId m_watchId = 0;
  Timer m_updateTimer;
  Timer m_deferredUpdateTimer;
  Timer m_imageReloadRetryTimer;
  std::function<bool()> m_updateDeferralCallback;
  InputArea* m_area = nullptr;
  Flex* m_flex = nullptr;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Label* m_label = nullptr;
  // Declarative mode: barWidget.render(tree) reconciles into m_uiHost and hides
  // the imperative glyph/image/label row.
  Flex* m_uiHost = nullptr;
  ui::UiTreeReconciler m_reconciler;
  std::optional<ui::UiTreeNode> m_tree;
  bool m_warnedImperativeWhileDeclarative = false;
  ScriptColorState m_textColor;
  ScriptColorState m_glyphColor;
  std::string m_imagePath;
  float m_imageWidth = 0.0f;
  float m_imageHeight = 0.0f;
  int m_updateIntervalMs = 250;
  std::uint32_t m_timerPhase = 0;
  std::uint64_t m_updateTimerGeneration = 0;
  int m_imageReloadRetries = 0;
  bool m_dirty = false;
  bool m_updateDeferred = false;
  bool m_isVertical = false;
  bool m_enableScroll = true;
  bool m_glyphVisible = false;
  bool m_imageWatch = false;
  bool m_imageDirty = false;
  bool m_imageForceReload = false;
  bool m_hasOnIpc = false;
  bool m_hasOnIpcKnown = false;
  FileWatcher::WatchId m_imageWatchId = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
