#include "shell/desktop/widgets/plugin_desktop_widget.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "scripting/plugin_runtime_context.h"
#include "ui/controls/flex.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

namespace {

  constexpr Logger kLog("plugin-desktop-widget");
  constexpr int kDefaultUpdateIntervalMs = 1000;

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

PluginDesktopWidget::PluginDesktopWidget(scripting::PluginRuntimeContext context, std::string outputName)
    : m_entryId(std::move(context.entryId)), m_sourcePath(std::move(context.sourcePath)),
      m_pluginDir(m_sourcePath.parent_path()), m_outputName(std::move(outputName)), m_scriptApi(context.scriptApi),
      m_settings(std::move(context.settings)), m_fileWatcher(context.fileWatcher), m_httpClient(context.httpClient),
      m_clipboard(context.clipboard) {
  scripting::PluginIpcRouter::instance().registerEndpoint(this);
}

PluginDesktopWidget::~PluginDesktopWidget() {
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

void PluginDesktopWidget::create() {
  auto flex = std::make_unique<Flex>();
  flex->setDirection(FlexDirection::Vertical);
  m_flex = flex.get();
  setRoot(std::move(flex));

  m_reconciler.setCallbackSink([this](const ui::UiTreeReconciler::ControlCallback& callback) {
    if (m_runtime != nullptr) {
      (void)m_runtime->enqueueCallStrings(
          callback.fn, callback.arg1, callback.arg2, makeScriptSnapshot(), callback.coalesce
      );
    }
  });
  m_reconciler.setPathResolver([this](const std::string& path) { return resolvePluginPath(path); });

  if (m_sourcePath.empty()) {
    kLog.warn("plugin desktop widget '{}': no source path", m_entryId);
    return;
  }
  std::string source = readFile(m_sourcePath);
  if (source.empty()) {
    kLog.warn("plugin desktop widget '{}': failed to read '{}'", m_entryId, m_sourcePath.string());
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
  startUpdateTimer();
  setupScriptWatch();
}

void PluginDesktopWidget::doLayout(Renderer& renderer) {
  if (m_flex == nullptr) {
    return;
  }
  if (m_tree.has_value()) {
    m_reconciler.setScale(contentScale());
    (void)m_reconciler.reconcile(*m_flex, *m_tree, renderer);
    m_treeDirty = false;
  }
  m_flex->layout(renderer);
}

void PluginDesktopWidget::doUpdate(Renderer& renderer) {
  (void)renderer;
  // Host-driven update (second tick / minute boundary): give the script a tick.
  // A render() of an unchanged tree is a no-op, so this cannot loop.
  if (m_wantsSecondTicks && m_runtime != nullptr) {
    (void)m_runtime->enqueueUpdate(makeScriptSnapshot());
  }
}

void PluginDesktopWidget::onFrameTick(float deltaMs, Renderer& renderer) {
  (void)renderer;
  if (m_runtime == nullptr || !m_needsFrameTick) {
    return;
  }
  // Coalesced like onAudioSpectrum: a slow script only ever sees the latest frame.
  (void)m_runtime->enqueueCallStrings(
      "onFrameTick", std::format("{:.3f}", deltaMs), {}, makeScriptSnapshot(), /*coalesce=*/true
  );
  requestRedraw(); // keep the frame loop alive while animating
}

PluginDesktopWidget::DispatchResult PluginDesktopWidget::dispatchIpc(
    std::string_view event, std::string_view payload, const scripting::ScriptSnapshot& snapshot
) {
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

void PluginDesktopWidget::handleScriptResult(scripting::ScriptResult result) {
  if (result.hasOnIpcKnown) {
    m_hasOnIpc = result.hasOnIpc;
    m_hasOnIpcKnown = true;
  }

  if (result.unhealthy) {
    m_updateTimer.stop();
    kLog.warn("plugin desktop widget '{}' disabled after repeated timeouts", m_entryId);
  }

  const auto& patch = result.patch;
  if (patch.wantsSecondTicks.has_value()) {
    m_wantsSecondTicks = *patch.wantsSecondTicks;
  }
  if (patch.needsFrameTick.has_value()) {
    const bool was = m_needsFrameTick;
    m_needsFrameTick = *patch.needsFrameTick;
    if (m_needsFrameTick && !was) {
      requestFrameTick();
    }
  }
  if (patch.updateIntervalMs.has_value()) {
    m_updateIntervalMs = std::max(16, *patch.updateIntervalMs);
    startUpdateTimer();
  }
  if (patch.uiTree.has_value() && (!m_tree.has_value() || *patch.uiTree != *m_tree)) {
    m_tree = *patch.uiTree;
    m_treeDirty = true;
    requestLayout();
  }
}

scripting::ScriptSnapshot PluginDesktopWidget::makeScriptSnapshot() const {
  return scripting::ScriptSnapshot{
      .isVertical = false,
      .outputName = m_outputName,
      .barName = {},
      .focusedOutputName = {},
  };
}

std::string PluginDesktopWidget::resolvePluginPath(const std::string& path) const {
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

void PluginDesktopWidget::startUpdateTimer() {
  m_updateTimer.startRepeating(std::chrono::milliseconds(m_updateIntervalMs), [this] {
    if (m_runtime != nullptr) {
      (void)m_runtime->enqueueUpdate(makeScriptSnapshot());
    }
  });
}

void PluginDesktopWidget::setupScriptWatch() {
  if (m_sourcePath.empty() || m_fileWatcher == nullptr) {
    return;
  }
  m_watchId = m_fileWatcher->watch(m_sourcePath, [this] { reloadScript(); }, FileWatcher::WatchTrigger::WriteCompleted);
}

void PluginDesktopWidget::teardownScriptWatch() {
  if (m_watchId == 0 || m_fileWatcher == nullptr) {
    return;
  }
  m_fileWatcher->unwatch(m_watchId);
  m_watchId = 0;
}

void PluginDesktopWidget::reloadScript() {
  std::string source = readFile(m_sourcePath);
  auto name = m_sourcePath.filename().string();
  if (source.empty() || m_runtime == nullptr) {
    kLog.warn("hot reload: failed to reload '{}'", name);
    notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    return;
  }

  // Tick opt-ins and interval reset to defaults; the reloaded script re-declares
  // them. The current tree stays visible until the new render() lands.
  m_wantsSecondTicks = false;
  m_needsFrameTick = false;
  m_updateIntervalMs = kDefaultUpdateIntervalMs;
  m_hasOnIpc = false;
  m_hasOnIpcKnown = false;

  m_runtime->reload(m_sourcePath.string(), std::move(source), makeScriptSnapshot());
  startUpdateTimer();
  requestRedraw();
  kLog.info("hot reload: reloaded '{}'", name);
  notify::info("Noctalia", i18n::tr("bar.widgets.scripted.reloaded"), name);
}
