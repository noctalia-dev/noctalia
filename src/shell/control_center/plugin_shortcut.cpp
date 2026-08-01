#include "shell/control_center/plugin_shortcut.h"

#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/panel/panel_manager.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <utility>

namespace {
  constexpr Logger kLog("plugin-shortcut");

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
      return {};
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
  }
} // namespace

PluginShortcut::PluginShortcut(scripting::PluginRuntimeContext context)
    : m_entryId(std::move(context.entryId)), m_sourcePath(std::move(context.sourcePath)),
      m_pluginDir(m_sourcePath.parent_path()), m_settings(std::move(context.settings)), m_scriptApi(context.scriptApi),
      m_fileWatcher(context.fileWatcher), m_httpClient(context.httpClient), m_clipboard(context.clipboard),
      m_platform(context.platform) {
  start();
}

PluginShortcut::~PluginShortcut() {
  if (m_alive) {
    *m_alive = false;
  }
  teardownScriptWatch();
  if (m_runtime != nullptr) {
    if (m_subscription != 0) {
      m_runtime->unsubscribe(m_subscription);
    }
    m_runtime->stop();
  }
}

void PluginShortcut::start() {
  std::string code = readFile(m_sourcePath);
  if (code.empty()) {
    kLog.warn("shortcut '{}': empty or unreadable source {}", m_entryId, m_sourcePath.string());
    return;
  }
  m_runtime = std::make_shared<scripting::ScriptRuntime>(
      m_entryId, m_settings, m_scriptApi, m_pluginDir, m_httpClient, m_clipboard
  );

  auto alive = std::weak_ptr<bool>(m_alive);
  m_subscription = m_runtime->subscribe([this, alive](const scripting::ScriptResult& result) {
    auto token = alive.lock();
    if (token == nullptr || !*token) {
      return;
    }
    if (result.modulePathsKnown) {
      m_scriptWatcher.setModulePaths(result.modulePaths);
      recordLoadedModuleMtimes(result.modulePaths);
    }
    handleResult(result);
  });

  m_runtime->start(m_sourcePath.string(), std::move(code), makeScriptSnapshot());
  recordLoadedSourceMtime();
  armTimer();
  setupScriptWatch();
}

void PluginShortcut::setupScriptWatch() {
  m_scriptWatcher.start(m_fileWatcher, m_sourcePath, [this] { reloadScript(); });
}

void PluginShortcut::teardownScriptWatch() { m_scriptWatcher.stop(); }

void PluginShortcut::onPanelClose() {
  m_updateTimer.stop();
  teardownScriptWatch();
}

void PluginShortcut::onPanelOpen() {
  if (m_runtime == nullptr) {
    return;
  }
  if (sourceChangedSinceLoad()) {
    reloadScript(false);
  } else {
    armTimer();
  }
  setupScriptWatch();
}

void PluginShortcut::recordLoadedSourceMtime() {
  std::error_code ec;
  m_loadedSourceMtime = std::filesystem::last_write_time(m_sourcePath, ec);
  if (ec) {
    m_loadedSourceMtime = {};
  }
}

void PluginShortcut::recordLoadedModuleMtimes(std::span<const std::filesystem::path> paths) {
  m_loadedModuleMtimes.clear();
  for (const auto& path : paths) {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    // Only a module the VM actually loaded gets here, so a stat failure means it
    // was removed while the panel was closed — reload to surface that.
    m_loadedModuleMtimes.insert_or_assign(path, ec ? std::filesystem::file_time_type{} : mtime);
  }
}

bool PluginShortcut::sourceChangedSinceLoad() const {
  if (m_sourcePath.empty() || m_loadedSourceMtime == std::filesystem::file_time_type{}) {
    return false;
  }
  std::error_code ec;
  const auto current = std::filesystem::last_write_time(m_sourcePath, ec);
  if (ec) {
    return false;
  }
  if (current != m_loadedSourceMtime) {
    return true;
  }
  for (const auto& [path, loadedMtime] : m_loadedModuleMtimes) {
    std::error_code moduleEc;
    const auto moduleMtime = std::filesystem::last_write_time(path, moduleEc);
    if (moduleEc) {
      // Unreadable now but readable at load: a real change. Unreadable at load too:
      // nothing new to react to, so don't reload on every panel open.
      if (loadedMtime != std::filesystem::file_time_type{}) {
        return true;
      }
      continue;
    }
    if (moduleMtime != loadedMtime) {
      return true;
    }
  }
  return false;
}

void PluginShortcut::resetPresentation() {
  m_label.clear();
  m_iconOn = "circle";
  m_iconOff = "circle";
  m_active = false;
  m_enabled = true;
  m_updateIntervalMs = 1000;
}

void PluginShortcut::reloadScript(bool notifyUser) {
  std::string code = readFile(m_sourcePath);
  auto name = m_sourcePath.filename().string();
  if (code.empty()) {
    kLog.warn("shortcut '{}': failed to reload '{}'", m_entryId, m_sourcePath.string());
    if (notifyUser) {
      notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    }
    return;
  }
  if (m_runtime == nullptr) {
    kLog.warn("shortcut '{}': runtime unavailable for reload", m_entryId);
    if (notifyUser) {
      notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    }
    return;
  }

  m_updateTimer.stop();
  resetPresentation();
  m_runtime->reload(m_sourcePath.string(), std::move(code), makeScriptSnapshot());
  recordLoadedSourceMtime();
  armTimer();
  PanelManager::instance().refresh();
  kLog.info("hot reload: reloaded shortcut '{}'", m_entryId);
  if (notifyUser) {
    notify::info("Noctalia", i18n::tr("bar.widgets.scripted.reloaded"), name);
  }
}

void PluginShortcut::handleResult(const scripting::ScriptResult& result) {
  const auto& patch = result.patch;
  bool changed = false;
  if (patch.label.has_value() && *patch.label != m_label) {
    m_label = *patch.label;
    changed = true;
  }
  if (patch.iconOn.has_value() && *patch.iconOn != m_iconOn) {
    m_iconOn = *patch.iconOn;
    changed = true;
  }
  if (patch.iconOff.has_value() && *patch.iconOff != m_iconOff) {
    m_iconOff = *patch.iconOff;
    changed = true;
  }
  if (patch.active.has_value() && *patch.active != m_active) {
    m_active = *patch.active;
    changed = true;
  }
  if (patch.enabled.has_value() && *patch.enabled != m_enabled) {
    m_enabled = *patch.enabled;
    changed = true;
  }
  if (patch.updateIntervalMs.has_value()) {
    const int next = std::max(16, *patch.updateIntervalMs);
    if (next != m_updateIntervalMs) {
      m_updateIntervalMs = next;
      armTimer();
    }
  }
  if (changed) {
    // The control center polls our state each update pass; kick one.
    PanelManager::instance().refresh();
  }
}

void PluginShortcut::armTimer() {
  m_updateTimer.stop();
  auto alive = std::weak_ptr<bool>(m_alive);
  m_updateTimer.startRepeating(std::chrono::milliseconds(m_updateIntervalMs), [this, alive] {
    auto token = alive.lock();
    if (token != nullptr && *token && m_runtime != nullptr) {
      (void)m_runtime->enqueueUpdate(makeScriptSnapshot());
    }
  });
}

void PluginShortcut::onClick() {
  if (m_runtime != nullptr) {
    (void)m_runtime->enqueueCall("onClick", makeScriptSnapshot());
  }
}

void PluginShortcut::onRightClick() {
  if (m_runtime != nullptr) {
    (void)m_runtime->enqueueCall("onRightClick", makeScriptSnapshot());
  }
}

scripting::ScriptSnapshot PluginShortcut::makeScriptSnapshot() const {
  scripting::ScriptSnapshot snapshot;
  snapshot.focusedOutputName = focusedOutputName();
  return snapshot;
}

std::string PluginShortcut::focusedOutputName() const {
  if (m_platform == nullptr) {
    return {};
  }
  wl_output* output = m_platform->preferredInteractiveOutput();
  const auto* info = m_platform->findOutputByWl(output);
  return info != nullptr ? info->connectorName : std::string{};
}
