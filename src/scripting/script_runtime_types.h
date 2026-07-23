#pragma once

#include "config/config_types.h"
#include "core/process/process.h"
#include "scripting/script_arg.h"
#include "ui/ui_tree.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace scripting {

  using ScriptSettings = std::unordered_map<std::string, WidgetSettingValue>;

  struct ScriptColorPatch {
    std::string role;
    std::string mode;

    bool operator==(const ScriptColorPatch&) const = default;
  };

  struct ScriptImagePatch {
    std::string path;
    bool watch = false;
    float width = 0.0f;
    float height = 0.0f;

    bool operator==(const ScriptImagePatch&) const = default;
  };

  struct ScriptTooltipRowPatch {
    std::string key;
    std::string value;

    bool operator==(const ScriptTooltipRowPatch&) const = default;
  };

  struct ScriptTooltipPatch {
    bool clear = false;
    std::string text;
    std::vector<ScriptTooltipRowPatch> rows;

    [[nodiscard]] bool hasRows() const noexcept { return !rows.empty(); }

    bool operator==(const ScriptTooltipPatch&) const = default;
  };

  // One launcher result published by a [[launcher_provider]] entry's onQuery.
  struct ScriptLauncherResult {
    std::string id;
    std::string title;
    std::string subtitle;
    std::string glyph;
    std::string icon;
    std::string badge;
    std::string category;
    std::string presentation;
    std::optional<std::string> query;
    double score = 0.0;

    bool operator==(const ScriptLauncherResult&) const = default;
  };

  // The full result set for a single query. `query` echoes the text onQuery was
  // answering, so the provider can map late async results back to the right query.
  struct ScriptLauncherResultSet {
    std::string query;
    std::vector<ScriptLauncherResult> results;

    bool operator==(const ScriptLauncherResultSet&) const = default;
  };

  struct ScriptPatch {
    std::optional<std::string> text;
    std::optional<std::string> glyph;
    std::optional<ScriptImagePatch> image;
    std::optional<ScriptTooltipPatch> tooltip;
    std::optional<std::string> fontFamily;
    std::optional<std::string> fontBaseline;
    std::optional<ScriptColorPatch> textColor;
    std::optional<ScriptColorPatch> glyphColor;
    std::optional<bool> visible;
    std::optional<int> updateIntervalMs;

    // Control-center shortcut fields (the `shortcut.*` namespace).
    std::optional<std::string> label;
    std::optional<std::string> iconOn;
    std::optional<std::string> iconOff;
    std::optional<bool> active;
    std::optional<bool> enabled;

    // Launcher-provider results (the `launcher.*` namespace).
    std::optional<ScriptLauncherResultSet> launcherResults;
    std::optional<std::string> launcherQuery;

    // Desktop-widget fields (the `desktopWidget.*` namespace): the declarative
    // control tree from desktopWidget.render() plus tick opt-ins.
    std::optional<ui::UiTreeNode> uiTree;
    std::optional<bool> wantsSecondTicks;
    std::optional<bool> needsFrameTick;

    // Panel field (the `panel.*` namespace): a close request.
    std::optional<bool> requestClose;

    [[nodiscard]] bool empty() const {
      return !text.has_value()
          && !glyph.has_value()
          && !image.has_value()
          && !tooltip.has_value()
          && !fontFamily.has_value()
          && !fontBaseline.has_value()
          && !textColor.has_value()
          && !glyphColor.has_value()
          && !visible.has_value()
          && !updateIntervalMs.has_value()
          && !label.has_value()
          && !iconOn.has_value()
          && !iconOff.has_value()
          && !active.has_value()
          && !enabled.has_value()
          && !launcherResults.has_value()
          && !launcherQuery.has_value()
          && !uiTree.has_value()
          && !wantsSecondTicks.has_value()
          && !needsFrameTick.has_value()
          && !requestClose.has_value();
    }
  };

  enum class ScriptSideEffectKind : std::uint8_t {
    Log,
    NotifyInfo,
    NotifyError,
    CopyToClipboard,
    SetWallpaperEnabled,
    SetWallpaper,
    TogglePanel,
  };

  struct ScriptSideEffect {
    ScriptSideEffectKind kind = ScriptSideEffectKind::Log;
    std::string title;
    std::string body;
    // SetWallpaperEnabled: title holds the output connector, flag the enabled state.
    // SetWallpaper: title holds the output connector (empty = all outputs), body the image path.
    // TogglePanel: title holds the panel id ("author/plugin:panel").
    bool flag = false;
  };

  struct ScriptSnapshot {
    bool isVertical = false;
    std::string outputName;
    std::string barName;
    std::string focusedOutputName;
  };

  enum class ScriptEventKind : std::uint8_t {
    Load,
    Reload,
    Update,
    Call,
    CallArgs,
    AsyncCommandResult,
    AsyncProcessMatchResult,
    AsyncHttpResult,
    ColorPickerResult,
    StateWatchResult,
    StreamLine,
    HttpStreamLine,
    HttpStreamClosed,
    SettingsChanged,
    Stop,
  };

  // Queue policy for a callback invocation.
  struct ScriptCallOptions {
    // A newer queued call to the same callback replaces this one.
    bool coalesce = false;
    // This call may be dropped when the queue is full.
    bool droppable = false;
  };

  struct ScriptEvent {
    ScriptEventKind kind = ScriptEventKind::Update;
    std::uint64_t generation = 0;
    std::uint64_t hostId = 0;
    std::string functionName;
    std::string chunkName;
    std::string source;
    // StreamLine / HttpStreamLine payload.
    std::string first;
    // CallArgs payload: the callback's argument list, pushed in order.
    ScriptArgs args;
    bool processMatchResult = false;
    // When true, a newer CallArgs event with the same functionName supersedes
    // this one while it is still queued (only the latest payload matters, e.g.
    // onAudioSpectrum frames). IPC and other callbacks leave this false so every
    // event is delivered.
    bool coalesce = false;
    // When true, this event may be dropped to make room once the queue is full
    // (state-echo callbacks such as onHover, where a missed edge is harmless).
    bool droppable = false;
    int callbackRef = 0;
    process::RunResult commandResult;
    // AsyncHttpResult / HttpStreamClosed payload.
    bool httpOk = false;
    bool httpIsDownload = false;
    int httpStatus = 0;
    std::string httpBody;
    // ColorPickerResult payload (nil on cancellation).
    std::optional<std::string> colorPickerResult;
    // StateWatchResult payload (the changed value as JSON).
    std::string stateJson;
    // Stop payload: SIGINT/SIGTERM for signal-driven process shutdown, otherwise 0.
    int exitSignal = 0;
    // SettingsChanged payload: the new seeded settings snapshot to swap in.
    ScriptSettings newSettings;
    ScriptSnapshot snapshot;
    std::chrono::milliseconds budget{12};
  };

  struct ScriptResult {
    std::uint64_t generation = 0;
    ScriptPatch patch;
    std::vector<ScriptSideEffect> sideEffects;
    bool ok = true;
    bool timedOut = false;
    bool hasOnIpc = false;
    bool hasOnIpcKnown = false;
    bool unhealthy = false;
    // True when this result included a CopyToClipboard side effect (before dispatch).
    bool copiedToClipboard = false;
    std::string callbackName;
    std::string error;
  };

  using ScriptResultCallback = std::function<void(ScriptResult)>;

} // namespace scripting
