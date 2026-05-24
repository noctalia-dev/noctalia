#pragma once

#include "config/config_service.h"
#include "core/file_watcher.h"
#include "core/timer_manager.h"
#include "scripting/script_runtime.h"
#include "shell/bar/widget.h"
#include "ui/palette.h"

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
class InputArea;
class Label;
class CompositorPlatform;
class ClipboardService;
class MprisService;
class PipeWireSpectrum;

class ScriptedWidget : public Widget {
public:
  enum class IpcDispatchResult {
    Handled,
    MissingHost,
    MissingCallback,
    Failed,
  };

  explicit ScriptedWidget(
      std::string configName, std::string scriptPath, std::string barName, std::string outputName,
      const WidgetConfig* config = nullptr, FileWatcher* fileWatcher = nullptr, CompositorPlatform* platform = nullptr,
      ClipboardService* clipboard = nullptr, PipeWireSpectrum* audioSpectrum = nullptr, MprisService* mpris = nullptr
  );
  ~ScriptedWidget() override;

  void create() override;

  void luaSetText(std::string_view text);
  void luaSetGlyph(std::string_view name);
  void luaSetFont(std::string_view familyOrPath);
  void luaSetColor(std::string_view role, std::string_view mode);
  void luaSetGlyphColor(std::string_view role, std::string_view mode);
  void luaSetVisible(bool visible);
  void luaSetUpdateInterval(float ms);
  void setUpdateDeferralCallback(std::function<bool()> callback);
  [[nodiscard]] IpcDispatchResult dispatchIpcEvent(std::string_view event, std::string_view payload);
  [[nodiscard]] bool isVertical() const { return m_isVertical; }

  [[nodiscard]] const std::unordered_map<std::string, WidgetSettingValue>& settings() const { return m_settings; }

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

  void reloadScript();
  void handleScriptResult(scripting::ScriptWidgetResult result);
  void applyScriptPatch(const scripting::ScriptWidgetPatch& patch);
  [[nodiscard]] scripting::ScriptWidgetSnapshot makeScriptSnapshot() const;
  [[nodiscard]] std::string focusedOutputName() const;
  void setupAudioSpectrum();
  void teardownAudioSpectrum();
  void handleAudioSpectrumChanged();
  void setupScriptWatch();
  void teardownScriptWatch();
  void startUpdateTimer();
  void armDeferredUpdate(std::uint64_t generation);
  void handleUpdateTimer();
  [[nodiscard]] std::chrono::milliseconds initialUpdateDelay(std::chrono::milliseconds interval) const noexcept;
  void runScriptUpdate();
  void scheduleDeferredUpdate();
  [[nodiscard]] bool shouldDeferUpdate() const;

  std::string m_scriptPath;
  std::string m_widgetConfigName;
  std::string m_barName;
  std::string m_outputName;
  std::filesystem::path m_resolvedPath;
  std::unordered_map<std::string, WidgetSettingValue> m_settings;
  std::shared_ptr<scripting::ScriptRuntime> m_runtime;
  scripting::ScriptRuntime::SubscriberId m_runtimeSubscription = 0;
  FileWatcher* m_fileWatcher = nullptr;
  CompositorPlatform* m_platform = nullptr;
  ClipboardService* m_clipboard = nullptr;
  PipeWireSpectrum* m_audioSpectrum = nullptr;
  MprisService* m_mpris = nullptr;
  FileWatcher::WatchId m_watchId = 0;
  Timer m_updateTimer;
  Timer m_deferredUpdateTimer;
  std::function<bool()> m_updateDeferralCallback;
  InputArea* m_area = nullptr;
  Flex* m_flex = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  ScriptColorState m_textColor;
  ScriptColorState m_glyphColor;
  int m_updateIntervalMs = 250;
  std::uint32_t m_timerPhase = 0;
  std::uint64_t m_updateTimerGeneration = 0;
  std::uint64_t m_audioSpectrumListenerId = 0;
  int m_audioSpectrumBands = 16;
  bool m_dirty = false;
  bool m_updateDeferred = false;
  bool m_isVertical = false;
  bool m_glyphVisible = false;
  bool m_hotReload = false;
  bool m_sharedScope = false;
  bool m_audioSpectrumEnabled = false;
  bool m_hasOnIpc = false;
  bool m_hasOnIpcKnown = false;
  bool m_fontConfigDirty = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
