#include "shell/panel/plugin_panel.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/scene/node.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/panel/panel_manager.h"
#include "ui/controls/flex.h"

#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace {

  constexpr Logger kLog("plugin-panel");
  constexpr int kTickIntervalMs = 1000;
  constexpr float kDefaultPanelWidth = 480.0f;
  constexpr float kDefaultPanelHeight = 400.0f;

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
      m_pluginDir(m_sourcePath.parent_path()), m_scriptApi(context.scriptApi), m_settings(std::move(context.settings)),
      m_fileWatcher(context.fileWatcher), m_httpClient(context.httpClient), m_clipboard(context.clipboard),
      m_preferredWidth(options.width > 0.0 ? static_cast<float>(options.width) : kDefaultPanelWidth),
      m_preferredHeight(options.height > 0.0 ? static_cast<float>(options.height) : kDefaultPanelHeight),
      m_widthFill(options.widthFill), m_heightFill(options.heightFill),
      m_dismissOnOutsideClick(options.dismissOnOutsideClick), m_shellConfig(options.shellConfig) {
  scripting::PluginIpcRouter::instance().registerEndpoint(this);
}

PluginPanel::~PluginPanel() {
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

  auto flex = std::make_unique<Flex>();
  flex->setDirection(FlexDirection::Vertical);
  flex->setAlign(FlexAlign::Stretch);
  m_flex = flex.get();

  auto content = std::make_unique<Flex>();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setFlexGrow(1.0f);
  m_contentFlex = static_cast<Flex*>(flex->addChild(std::move(content)));

  auto overlay = std::make_unique<Node>();
  overlay->setParticipatesInLayout(false);
  overlay->setHitTestVisible(false);
  overlay->setZIndex(std::numeric_limits<std::int32_t>::max());
  m_dragOverlay = flex->addChild(std::move(overlay));
  flex->setAnimationManager(m_animations);

  setRoot(std::move(flex));
  m_treeDirty = true;

  m_reconciler.setCallbackSink([this](const ui::UiTreeReconciler::ControlCallback& callback) {
    if (m_runtime != nullptr) {
      (void)m_runtime->enqueueCallStrings(
          callback.fn, callback.arg1, callback.arg2, makeScriptSnapshot(), callback.coalesce
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
    handleScriptResult(std::move(result));
  });

  m_runtime->start(m_sourcePath.string(), std::move(source), makeScriptSnapshot());
}

void PluginPanel::onOpen(std::string_view context) {
  m_open = true;
  if (m_runtime != nullptr) {
    (void)m_runtime->enqueueCallStrings("onOpen", std::string(context), {}, makeScriptSnapshot());
  }
  startTickTimer();
}

void PluginPanel::onClose() {
  m_open = false;
  m_tickTimer.stop();
  // The scene (including the overlay node) is torn down after close; cancel any
  // active drag and detach the overlay while the tree is still alive so the
  // controller never holds a dangling overlay root between close and reopen.
  m_reconciler.setDragDropOverlayRoot(nullptr);
  if (m_runtime != nullptr) {
    (void)m_runtime->enqueueCall("onClose", makeScriptSnapshot());
  }
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
    m_dragOverlay->setPosition(0.0f, 0.0f);
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
    kLog.warn("plugin panel '{}' disabled after repeated timeouts", m_entryId);
  }

  const auto& patch = result.patch;
  if (patch.wantsSecondTicks.has_value()) {
    m_wantsSecondTicks = *patch.wantsSecondTicks;
    startTickTimer();
  }
  if (patch.requestClose.value_or(false)) {
    PanelManager::instance().closePanel();
    return;
  }
  if (patch.uiTree.has_value() && (!m_tree.has_value() || *patch.uiTree != *m_tree)) {
    m_tree = *patch.uiTree;
    m_treeDirty = true;
    if (PanelManager::instance().isOpenPanel(m_entryId)) {
      PanelManager::instance().refresh();
    }
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
  if (m_sourcePath.empty() || m_fileWatcher == nullptr) {
    return;
  }
  m_watchId = m_fileWatcher->watch(m_sourcePath, [this] { reloadScript(); }, FileWatcher::WatchTrigger::WriteCompleted);
}

void PluginPanel::teardownScriptWatch() {
  if (m_watchId == 0 || m_fileWatcher == nullptr) {
    return;
  }
  m_fileWatcher->unwatch(m_watchId);
  m_watchId = 0;
}

void PluginPanel::reloadScript() {
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
