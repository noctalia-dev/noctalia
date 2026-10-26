#pragma once

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_layer_config.h"
#include "scripting/plugin_script_watcher.h"
#include "scripting/script_runtime.h"
#include "ui/ui_tree.h"
#include "ui/ui_tree_reconciler.h"
#include "wayland/layer_surface.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class ClipboardService;
class CompositorPlatform;
class ConfigService;
class FileWatcher;
class Flex;
class HttpClient;
class InputArea;
class Node;
class RenderContext;
class Renderer;
struct KeyboardEvent;
struct PointerEvent;
struct WaylandOutput;
struct wl_output;
struct wl_surface;

namespace scripting {
  struct PluginRuntimeContext;
  class ScriptApiContext;
} // namespace scripting

struct PluginLayerServices {
  CompositorPlatform& platform;
  RenderContext* renderContext = nullptr;
  ConfigService* config = nullptr;
};

// One output's surface of a plugin `[[layer]]` entry: a layer-shell surface configured from
// the entry's PluginLayerConfig, with its own Luau runtime that renders a declarative tree via
// `layer.render(...)`. Created when the layer is shown and destroyed when it is hidden.
class PluginLayer : public scripting::PluginIpcEndpoint {
public:
  PluginLayer(PluginLayerServices services, scripting::PluginRuntimeContext context, const WaylandOutput& output);
  ~PluginLayer() override;

  PluginLayer(const PluginLayer&) = delete;
  PluginLayer& operator=(const PluginLayer&) = delete;

  [[nodiscard]] bool initialize(const scripting::PluginLayerConfig& config);
  void applyConfig(const scripting::PluginLayerConfig& config);

  [[nodiscard]] wl_output* output() const noexcept { return m_output; }
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] bool pointerInside() const noexcept { return m_pointerInside; }
  void onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  void requestLayout();

  // PluginIpcEndpoint
  [[nodiscard]] std::string_view ipcEntryId() const override { return m_entryId; }
  [[nodiscard]] std::string_view ipcOutputName() const override { return m_outputName; }
  [[nodiscard]] std::string_view ipcBarName() const override { return {}; }
  [[nodiscard]] DispatchResult
  dispatchIpc(std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot) override;

private:
  void buildScene();
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void layoutScene(Renderer& renderer);
  void syncAfterInput();
  void startScript();
  void reloadScript();
  void startUpdateTimer();
  void handleScriptResult(scripting::ScriptResult result);
  [[nodiscard]] scripting::ScriptSnapshot makeScriptSnapshot() const;
  [[nodiscard]] std::string resolvePluginPath(const std::string& path) const;
  [[nodiscard]] float contentScale() const;
  // Requested surface extent on one axis: 0 when stretched between both anchored edges,
  // the configured size when set, otherwise the measured content size.
  [[nodiscard]] std::uint32_t requestedExtent(std::uint32_t configured, bool stretched, float content) const;

  PluginLayerServices m_services;
  std::string m_entryId; // "author/plugin:entry"
  std::filesystem::path m_sourcePath;
  std::filesystem::path m_pluginDir;
  scripting::ScriptApiContext& m_scriptApi;
  scripting::ScriptSettings m_settings;
  FileWatcher* m_fileWatcher = nullptr;
  HttpClient* m_httpClient = nullptr;
  ClipboardService* m_clipboard = nullptr;
  wl_output* m_output = nullptr;
  std::string m_outputName;
  scripting::PluginLayerConfig m_config;

  std::shared_ptr<scripting::ScriptRuntime> m_runtime;
  scripting::ScriptRuntime::SubscriberId m_runtimeSubscription = 0;
  scripting::PluginScriptWatcher m_scriptWatcher;
  Timer m_updateTimer;
  int m_updateIntervalMs = 1000;
  bool m_needsFrameTick = false;
  bool m_hasOnIpc = false;
  bool m_hasOnIpcKnown = false;

  ui::UiTreeReconciler m_reconciler;
  std::optional<ui::UiTreeNode> m_tree;
  bool m_treeDirty = false;
  float m_contentWidth = 0.0F;
  float m_contentHeight = 0.0F;
  bool m_pointerInside = false;
  InputArea* m_pendingFocusArea = nullptr;

  // Declared before the scene: ~Node() calls cancelForOwner() on the manager, so the scene must die first.
  AnimationManager m_animations;
  InputDispatcher m_inputDispatcher;
  std::unique_ptr<LayerSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  Flex* m_flex = nullptr;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
