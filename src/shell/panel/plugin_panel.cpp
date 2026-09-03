#include "shell/panel/plugin_panel.h"

#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/scene/node.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/flex.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace {

  constexpr Logger kLog("plugin-panel");
  constexpr int kTickIntervalMs = 1000;
  constexpr float kDefaultPanelWidth = 480.0F;
  constexpr float kDefaultPanelHeight = 400.0F;

  // Manifest vocabulary (scripting::kPanelKeyboardFocusModes) to layer-shell mode.
  // The manifest parser rejects anything else, so an unknown token here means the
  // two lists drifted apart.
  LayerShellKeyboard keyboardModeFromManifest(std::string_view value) {
    if (value == "none") {
      return LayerShellKeyboard::None;
    }
    if (value == "exclusive") {
      return LayerShellKeyboard::Exclusive;
    }
    if (value != "on_demand") {
      kLog.warn("unknown keyboard_focus \"{}\", using on_demand", value);
    }
    return LayerShellKeyboard::OnDemand;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) {
      return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

} // namespace

PluginPanel::PluginPanel(scripting::PluginRuntimeContext context, PluginPanelOptions options)
    : m_entryId(std::move(context.entryId)), m_sourcePath(std::move(context.sourcePath)),
      m_pluginDir(std::move(context.pluginDir)), m_scriptApi(context.scriptApi),
      m_settings(std::move(context.settings)), m_fileWatcher(context.fileWatcher), m_httpClient(context.httpClient),
      m_clipboard(context.clipboard),
      m_preferredWidth(options.width > 0.0 ? static_cast<float>(options.width) : kDefaultPanelWidth),
      m_preferredHeight(options.height > 0.0 ? static_cast<float>(options.height) : kDefaultPanelHeight),
      m_widthFill(options.widthFill), m_heightFill(options.heightFill),
      m_dismissOnOutsideClick(options.dismissOnOutsideClick),
      m_keyboardMode(keyboardModeFromManifest(options.keyboardFocus)),
      m_layer(layerShellLayerFromConfig(options.shellConfig.layer)), m_persistent(options.persistent),
      m_shellConfig(options.shellConfig) {
  // The manifest parser already validated every spec, so a parse failure here means the two
  // drifted apart. Skip the entry rather than capture a chord nobody can describe.
  m_captureKeys.reserve(options.captureKeys.size());
  for (const std::string& spec : options.captureKeys) {
    if (const auto chord = parseKeyChordSpec(spec); chord.has_value()) {
      m_captureKeys.push_back(CaptureKey{.chord = *chord, .spec = spec});
    } else {
      kLog.warn("{}: ignoring unparseable capture_keys entry \"{}\"", m_entryId, spec);
    }
  }
  scripting::PluginIpcRouter::instance().registerEndpoint(this);
}

bool PluginPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  // Without a handler there is nothing to deliver to, so leave the key to the host rather than
  // swallow it invisibly. Also false until the script has loaded, which is the right answer:
  // the panel is not interactive yet either.
  if (m_captureKeys.empty() || preedit || m_runtime == nullptr || !m_runtime->hasOnKey()) {
    return false;
  }
  // Cancel stays with the host: a panel that captured it could make itself impossible to
  // dismiss from the keyboard. Checked live because the user can rebind the action.
  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    return false;
  }

  const auto match = std::ranges::find_if(m_captureKeys, [sym, modifiers](const CaptureKey& candidate) {
    return keyChordMatches(candidate.chord, sym, modifiers);
  });
  if (match == m_captureKeys.end()) {
    return false;
  }

  // Still consumed, so the repeat does not leak to the host, but the script sees one press per
  // physical press.
  if (pressed && match->held) {
    return true;
  }
  match->held = pressed;

  // The runtime is off-thread, so the script cannot answer "did you consume this?" in time.
  // Declaring the chord in capture_keys is the consume decision; the call is fire-and-forget.
  (void)m_runtime->enqueueCallArgs("onKey", scripting::ScriptArgs{match->spec, pressed}, makeScriptSnapshot());
  return true;
}

void PluginPanel::releaseCapturedKeys() {
  // A panel can close with a key still down, in which case no release ever arrives. Clearing
  // here keeps the next open from treating the first press as a repeat and dropping it.
  for (CaptureKey& key : m_captureKeys) {
    key.held = false;
  }
}

PluginPanel::~PluginPanel() {
  closeContextMenu();
  scripting::PluginIpcRouter::instance().unregisterEndpoint(this);
  if (m_alive) {
    *m_alive = false;
  }
  teardownScriptWatch();
  if (m_runtime != nullptr) {
    if (m_runtimeSubscription != 0) {
      m_runtime->unsubscribe(m_runtimeSubscription);
    }
    m_runtime->stop();
  }
}

void PluginPanel::create() {
  // PanelManager calls create() on every open and releases the root into the
  // scene, which is torn down on close — so the root Flex is rebuilt each open.
  // The reconciler's retained slots point into the previous (now-freed) tree, so
  // reset it to rebuild the retained m_tree from scratch into the fresh Flex.
  m_reconciler.reset();
  m_reconciler.setDragDropOverlayRoot(nullptr);

  auto flex = ui::column(
      {
          .out = &m_flex,
          .align = FlexAlign::Stretch,
          .configure = [this](Flex& root) { root.setAnimationManager(m_animations); },
      },
      ui::column({
          .out = &m_contentFlex,
          .align = FlexAlign::Stretch,
          .flexGrow = 1.0F,
      }),
      ui::node({
          .out = &m_dragOverlay,
          .participatesInLayout = false,
          .configure = [](Node& overlay) {
            overlay.setHitTestVisible(false);
            overlay.setZIndex(std::numeric_limits<std::int32_t>::max());
          },
      })
  );

  setRoot(std::move(flex));
  m_treeDirty = true;

  m_reconciler.setCallbackSink([this](const ui::UiTreeReconciler::ControlCallback& callback) {
    if (m_runtime != nullptr) {
      auto snapshot = makeScriptSnapshot();
      if (callback.pointerContext.has_value()) {
        snapshot.pointerContext = scripting::ScriptPointerContext{
            .x = callback.pointerContext->x,
            .y = callback.pointerContext->y,
            .serial = callback.pointerContext->serial,
            .time = callback.pointerContext->time,
            .surfaceGeneration = m_openGeneration,
        };
      }
      (void)m_runtime->enqueueCallStrings(
          callback.fn, callback.arg1, callback.arg2, std::move(snapshot), callback.coalesce, callback.coalesceKey
      );
    }
  });
  m_reconciler.setDragDropEnabled(true);
  m_reconciler.setDragDropOverlayRoot(m_dragOverlay);
  m_reconciler.setPathResolver([this](const std::string& path) { return resolvePluginPath(path); });
  m_pendingFocusArea = nullptr;
  m_reconciler.setFocusRequestSink([this](InputArea* area) { m_pendingFocusArea = area; });

  // The runtime and file watch are set up once and persist across open/close, so
  // plugin state survives reopening and watches aren't duplicated.
  if (m_runtime == nullptr) {
    startScript();
    setupScriptWatch();
  }
}

void PluginPanel::startScript() {
  if (m_sourcePath.empty()) {
    kLog.warn("plugin panel '{}': no source path", m_entryId);
    return;
  }
  std::string source = readFile(m_sourcePath);
  if (source.empty()) {
    kLog.warn("plugin panel '{}': failed to read '{}'", m_entryId, m_sourcePath.string());
    return;
  }

  m_runtime = std::make_shared<scripting::ScriptRuntime>(
      m_entryId, m_settings, m_scriptApi, m_pluginDir, m_httpClient, m_clipboard
  );

  auto alive = std::weak_ptr<bool>(m_alive);
  m_runtimeSubscription = m_runtime->subscribe([this, alive](scripting::ScriptResult result) {
    auto token = alive.lock();
    if (token == nullptr || !*token) {
      return;
    }
    if (result.modulePathsKnown) {
      m_scriptWatcher.setModulePaths(result.modulePaths);
    }
    handleScriptResult(std::move(result));
  });

  m_runtime->start(m_sourcePath.string(), std::move(source), makeScriptSnapshot());
}

void PluginPanel::onOpen(std::string_view context) {
  closeContextMenu();
  ++m_openGeneration;
  m_open = true;
  m_openContext = std::string(context);
  if (m_runtime != nullptr) {
    (void)m_runtime->enqueueCallStrings("onOpen", std::string(context), {}, makeScriptSnapshot());
  }
  startTickTimer();
  if (m_needsFrameTick) {
    PanelManager::instance().requestAnimationFrameForPanel(m_entryId);
  }
}

bool PluginPanel::isContextActive(std::string_view context) const { return m_open && m_openContext == context; }

void PluginPanel::onClose() {
  ++m_openGeneration;
  m_open = false;
  m_openContext.clear();
  closeContextMenu();
  m_tickTimer.stop();
  releaseCapturedKeys();
  // The scene (including the overlay node) is torn down after close; cancel any
  // active drag and detach the overlay while the tree is still alive so the
  // controller never holds a dangling overlay root between close and reopen.
  m_reconciler.setDragDropOverlayRoot(nullptr);
  if (m_runtime != nullptr) {
    (void)m_runtime->enqueueCall("onClose", makeScriptSnapshot());
  }
}

bool PluginPanel::dismissTransientUi() {
  if (m_contextMenuPopup == nullptr || !m_contextMenuPopup->isOpen()) {
    return false;
  }
  closeContextMenu();
  return true;
}

void PluginPanel::onFrameTick(float deltaMs) {
  if (m_runtime == nullptr || !m_needsFrameTick || !m_open) {
    return;
  }
  // Coalesced like the desktop-widget path: a slow script only ever sees the latest frame.
  (void)m_runtime->enqueueCallStrings(
      "onFrameTick", std::format("{:.3F}", deltaMs), {}, makeScriptSnapshot(), /*coalesce=*/true
  );
  // Keep the frame loop alive while animating.
  PanelManager::instance().requestAnimationFrameForPanel(m_entryId);
}

void PluginPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_flex == nullptr || m_contentFlex == nullptr) {
    return;
  }
  if (m_tree.has_value()) {
    m_reconciler.setScale(contentScale());
    (void)m_reconciler.reconcile(*m_contentFlex, *m_tree, renderer);
    m_treeDirty = false;
  }
  m_flex->setSize(width, height);
  m_flex->layout(renderer);
  if (m_dragOverlay != nullptr) {
    m_dragOverlay->setPosition(0.0F, 0.0F);
    m_dragOverlay->setFrameSize(width, height);
  }
}

void PluginPanel::doUpdate(Renderer& renderer) { (void)renderer; }

void PluginPanel::startTickTimer() {
  if (!m_wantsSecondTicks || !m_open) {
    m_tickTimer.stop();
    return;
  }
  m_tickTimer.startRepeating(std::chrono::milliseconds(kTickIntervalMs), [this] {
    if (m_runtime != nullptr) {
      (void)m_runtime->enqueueUpdate(makeScriptSnapshot());
    }
  });
}

PluginPanel::DispatchResult
PluginPanel::dispatchIpc(std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot) {
  (void)snapshot;
  if (m_runtime == nullptr) {
    return DispatchResult::MissingHost;
  }
  if (m_hasOnIpcKnown && !m_hasOnIpc) {
    return DispatchResult::MissingCallback;
  }
  if (!m_runtime->enqueueCallStrings("onIpc", std::string(event), std::string(payload), makeScriptSnapshot())) {
    return DispatchResult::Failed;
  }
  return DispatchResult::Handled;
}

void PluginPanel::handleScriptResult(scripting::ScriptResult result) {
  if (result.hasOnIpcKnown) {
    m_hasOnIpc = result.hasOnIpc;
    m_hasOnIpcKnown = true;
  }

  if (result.unhealthy) {
    m_tickTimer.stop();
    m_needsFrameTick = false;
    kLog.warn("plugin panel '{}' disabled after repeated timeouts", m_entryId);
  }

  const auto& patch = result.patch;
  if (patch.wantsSecondTicks.has_value()) {
    m_wantsSecondTicks = *patch.wantsSecondTicks;
    startTickTimer();
  }
  if (patch.needsFrameTick.has_value()) {
    const bool was = m_needsFrameTick;
    m_needsFrameTick = *patch.needsFrameTick;
    if (m_needsFrameTick && !was && m_open) {
      PanelManager::instance().requestAnimationFrameForPanel(m_entryId);
    }
  }
  if (patch.requestClose.value_or(false)) {
    closeContextMenu();
    PanelManager::instance().closePanelById(m_entryId);
    return;
  }
  if (patch.uiTree.has_value() && (!m_tree.has_value() || *patch.uiTree != *m_tree)) {
    m_tree = *patch.uiTree;
    m_treeDirty = true;
    PanelManager::instance().refreshPanel(m_entryId);
  }
  if (result.ok && result.contextMenuRequest.has_value()) {
    openContextMenu(std::move(*result.contextMenuRequest));
  }
}

void PluginPanel::openContextMenu(scripting::ScriptContextMenuRequest request) {
  if (!m_open
      || m_runtime == nullptr
      || request.pointer.serial == 0
      || request.pointer.surfaceGeneration != m_openGeneration) {
    return;
  }
  auto& panels = PanelManager::instance();
  auto parent = panels.popupParentContextForPanel(m_entryId);
  auto* renderContext = panels.renderContext();
  auto* wayland = panels.wayland();
  if (!parent.has_value() || renderContext == nullptr || wayland == nullptr) {
    return;
  }

  if (m_contextMenuPopup == nullptr) {
    m_contextMenuPopup = std::make_unique<ContextMenuPopup>(*wayland, *renderContext);
  }
  panels.configureContextMenuPopup(*m_contextMenuPopup);

  std::vector<ContextMenuControlEntry> entries;
  std::vector<std::string> actionIds;
  entries.reserve(request.items.size());
  actionIds.reserve(request.items.size());
  for (const auto& item : request.items) {
    const auto nativeId = static_cast<std::int32_t>(actionIds.size());
    ContextMenuControlEntry entry{
        .id = nativeId,
        .label = item.label,
        .enabled = item.enabled,
        .separator = item.kind == scripting::ScriptContextMenuItemKind::Separator,
        .header = item.kind == scripting::ScriptContextMenuItemKind::Header,
    };
    if (item.kind == scripting::ScriptContextMenuItemKind::Action) {
      actionIds.push_back(item.id);
    } else {
      entry.id = -1;
    }
    entries.push_back(std::move(entry));
  }

  const std::string callback = std::move(request.onActivate);
  const std::optional<scripting::ScriptArg> callbackContext = std::move(request.context);
  const std::weak_ptr<bool> alive = m_alive;
  const std::uint64_t openGeneration = m_openGeneration;
  m_contextMenuPopup->setOnActivate([this, alive, openGeneration, actionIds = std::move(actionIds), callback,
                                     callbackContext](const ContextMenuControlEntry& entry) {
    const auto token = alive.lock();
    if (token == nullptr || !*token || !m_open || m_openGeneration != openGeneration) {
      return;
    }
    if (m_runtime == nullptr || entry.id < 0 || static_cast<std::size_t>(entry.id) >= actionIds.size()) {
      return;
    }
    scripting::ScriptArgs args{actionIds[static_cast<std::size_t>(entry.id)]};
    if (callbackContext.has_value()) {
      args.push_back(*callbackContext);
    }
    (void)m_runtime->enqueueCallArgs(callback, std::move(args), makeScriptSnapshot());
  });
  wl_surface* const parentSurface = parent->surface;
  panels.beginAttachedPopup(parentSurface);
  m_contextMenuPopup->setOnDismissed([parentSurface] {
    if (auto* manager = PanelManager::current(); manager != nullptr) {
      manager->clearActivePopup();
      manager->endAttachedPopup(parentSurface);
    }
  });

  const auto anchorX = static_cast<std::int32_t>(std::lround(request.pointer.x));
  const auto anchorY = static_cast<std::int32_t>(std::lround(request.pointer.y));
  m_contextMenuPopup->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .maxMenuWidth = Style::menuAutoMaxWidth * contentScale(),
          .contentScale = contentScale(),
          .maxVisible = request.maxVisible,
          .anchor = PopupAnchorRect{.x = anchorX, .y = anchorY, .width = 1, .height = 1},
          .parent =
              PopupSurfaceParent{
                  .layerSurface = parent->layerSurface,
                  .xdgSurface = parent->xdgSurface,
                  .output = parent->output,
                  // ContextMenuPopup uses a non-null wlSurface to temporarily
                  // promote a keyboard-inert layer parent and restores None on
                  // dismissal. Panels already using OnDemand/Exclusive must
                  // keep their current mode untouched.
                  .wlSurface = m_keyboardMode == LayerShellKeyboard::None ? parent->surface : nullptr,
              },
          .pointerParentSurface = parent->surface,
          .inputSerial = request.pointer.serial,
      }
  );
  if (m_contextMenuPopup->isOpen()) {
    panels.setActivePopup(m_contextMenuPopup.get());
  } else {
    panels.endAttachedPopup(parentSurface);
  }
}

void PluginPanel::closeContextMenu() {
  if (m_contextMenuPopup != nullptr) {
    m_contextMenuPopup->close();
  }
}

scripting::ScriptSnapshot PluginPanel::makeScriptSnapshot() const {
  return scripting::ScriptSnapshot{
      .isVertical = false,
      .outputName = {},
      .barName = {},
      .focusedOutputName = {},
  };
}

std::string PluginPanel::resolvePluginPath(const std::string& path) const {
  if (path.empty()) {
    return {};
  }
  if (path[0] == '~') {
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      return std::string(home) + path.substr(1);
    }
    return path;
  }
  if (path[0] == '/') {
    return path;
  }
  return (m_pluginDir / path).string();
}

void PluginPanel::setupScriptWatch() {
  m_scriptWatcher.start(m_fileWatcher, m_sourcePath, [this] { reloadScript(); });
}

void PluginPanel::teardownScriptWatch() { m_scriptWatcher.stop(); }

void PluginPanel::reloadScript() {
  ++m_openGeneration;
  closeContextMenu();
  std::string source = readFile(m_sourcePath);
  auto name = m_sourcePath.filename().string();
  if (source.empty() || m_runtime == nullptr) {
    kLog.warn("hot reload: failed to reload '{}'", name);
    notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    return;
  }

  // Tick opt-in resets to default; the reloaded script re-declares it. The
  // current tree stays visible until the new render() lands.
  m_wantsSecondTicks = false;
  m_hasOnIpc = false;
  m_hasOnIpcKnown = false;

  m_runtime->reload(m_sourcePath.string(), std::move(source), makeScriptSnapshot());
  if (m_open) {
    (void)m_runtime->enqueueCallStrings("onOpen", std::string(pendingOpenContext()), {}, makeScriptSnapshot());
  }
  startTickTimer();
  kLog.info("hot reload: reloaded '{}'", name);
  notify::info("Noctalia", i18n::tr("bar.widgets.scripted.reloaded"), name);
}
