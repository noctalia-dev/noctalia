#include "launcher/plugin_launcher_provider.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "scripting/plugin_runtime_context.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <utility>

namespace {
  constexpr Logger kLog("plugin-launcher");

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

PluginLauncherProvider::PluginLauncherProvider(
    scripting::PluginRuntimeContext context, PluginLauncherProviderOptions options
)
    : m_entryId(std::move(context.entryId)), m_displayName(std::move(options.displayName)),
      m_sourcePath(std::move(context.sourcePath)), m_pluginDir(m_sourcePath.parent_path()),
      m_prefix(std::move(options.prefix)), m_glyph(std::move(options.glyph)), m_globalSearch(options.globalSearch),
      m_debounceMs(options.debounceMs), m_categories(std::move(options.categories)),
      m_settings(std::move(context.settings)), m_scriptApi(context.scriptApi), m_fileWatcher(context.fileWatcher),
      m_httpClient(context.httpClient), m_clipboard(context.clipboard) {}

PluginLauncherProvider::~PluginLauncherProvider() {
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

void PluginLauncherProvider::initialize() {
  std::string code = readFile(m_sourcePath);
  if (code.empty()) {
    kLog.warn("launcher provider '{}': empty or unreadable source {}", m_entryId, m_sourcePath.string());
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
    handleResult(result);
  });

  m_runtime->start(m_sourcePath.string(), std::move(code), {});
  setupScriptWatch();
}

void PluginLauncherProvider::reset() {
  m_queryTimer.stop();
  m_cache.clear();
  m_resultsQuery.clear();
  m_lastSentQuery.clear();
  m_pendingQuery.clear();
  m_hasSentInitial = false;
  m_pendingActivate = false;
  m_pendingActivateId.clear();
}

std::vector<LauncherResult> PluginLauncherProvider::query(std::string_view text) const {
  if (m_runtime == nullptr) {
    return m_cache;
  }
  const std::string q(text);
  m_pendingQuery = q;
  // Cached results already answer this exact query — nothing to fetch.
  if (m_hasSentInitial && q == m_resultsQuery) {
    m_queryTimer.stop();
    return m_cache;
  }
  // Debounce subsequent keystrokes so a network-backed provider isn't hit on every
  // character; the first query (panel open) fires immediately for a snappy first paint.
  if (m_debounceMs > 0 && m_hasSentInitial) {
    armQueryTimer();
  } else {
    dispatchQuery(q);
  }
  return m_cache;
}

void PluginLauncherProvider::dispatchQuery(const std::string& text) const {
  if (m_runtime == nullptr || (m_hasSentInitial && text == m_lastSentQuery)) {
    return;
  }
  m_hasSentInitial = true;
  m_lastSentQuery = text;
  // Coalesced: a newer queued onQuery supersedes this one while it waits. The empty
  // second arg is ignored by onQuery(text).
  (void)m_runtime->enqueueCallStrings("onQuery", text, std::string(), {}, /*coalesce=*/true);
}

void PluginLauncherProvider::armQueryTimer() const {
  m_queryTimer.stop();
  auto alive = std::weak_ptr<bool>(m_alive);
  m_queryTimer.start(std::chrono::milliseconds(m_debounceMs), [this, alive]() {
    auto token = alive.lock();
    if (token != nullptr && *token) {
      dispatchQuery(m_pendingQuery);
    }
  });
}

void PluginLauncherProvider::setupScriptWatch() {
  if (m_sourcePath.empty() || m_fileWatcher == nullptr) {
    return;
  }
  m_watchId = m_fileWatcher->watch(m_sourcePath, [this] { reloadScript(); }, FileWatcher::WatchTrigger::WriteCompleted);
}

void PluginLauncherProvider::teardownScriptWatch() {
  if (m_watchId == 0 || m_fileWatcher == nullptr) {
    return;
  }
  m_fileWatcher->unwatch(m_watchId);
  m_watchId = 0;
}

void PluginLauncherProvider::reloadScript() {
  std::string code = readFile(m_sourcePath);
  auto name = m_sourcePath.filename().string();
  if (code.empty()) {
    kLog.warn("launcher provider '{}': failed to reload '{}'", m_entryId, m_sourcePath.string());
    notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    return;
  }
  if (m_runtime == nullptr) {
    kLog.warn("launcher provider '{}': runtime unavailable for reload", m_entryId);
    notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
    return;
  }

  m_queryTimer.stop();
  reset();
  m_runtime->reload(m_sourcePath.string(), std::move(code), {});
  if (m_onResultsChanged) {
    m_onResultsChanged();
  }
  kLog.info("hot reload: reloaded launcher provider '{}'", m_entryId);
  notify::info("Noctalia", i18n::tr("bar.widgets.scripted.reloaded"), name);
}

bool PluginLauncherProvider::activate(const LauncherResult& result) {
  if (result.query.has_value()) {
    if (m_onQueryRequested) {
      m_onQueryRequested(*result.query);
    }
    return false;
  }
  // Defer the close until onActivate resolves: the handler may call
  // launcher.setQuery() to rewrite the input and keep the panel open. Only defer
  // when the plugin actually defines onActivate, otherwise no result comes back
  // and the panel would hang open.
  if (m_runtime != nullptr && m_runtime->hasOnActivate()) {
    m_pendingActivate = true;
    m_pendingActivateId = result.id;
    (void)m_runtime->enqueueCallStrings("onActivate", result.id, std::string(), {}, /*coalesce=*/false);
    return false;
  }
  return true; // No onActivate handler — nothing to run, close immediately.
}

void PluginLauncherProvider::handleResult(const scripting::ScriptResult& result) {
  if (result.patch.launcherQuery.has_value() && m_onQueryRequested) {
    m_onQueryRequested(*result.patch.launcherQuery);
  }
  if (m_pendingActivate && result.callbackName == "onActivate") {
    m_pendingActivate = false;
    // The handler rewrote the query → stay open (already applied above). Otherwise
    // the activation is terminal, so close the launcher and record the usage.
    if (!result.patch.launcherQuery.has_value() && m_onActivationDone) {
      m_onActivationDone(m_pendingActivateId);
    }
    m_pendingActivateId.clear();
  }
  if (!result.patch.launcherResults.has_value()) {
    return;
  }
  const auto& set = *result.patch.launcherResults;
  m_cache.clear();
  m_cache.reserve(set.results.size());
  for (const auto& r : set.results) {
    LauncherResult lr;
    lr.id = r.id;
    lr.title = r.title.empty() ? r.id : r.title;
    lr.subtitle = r.subtitle;
    lr.glyphName = r.glyph;
    lr.iconName = r.icon;
    lr.badge = r.badge;
    lr.category = r.category;
    lr.presentation = r.presentation;
    lr.query = r.query;
    lr.score = r.score;
    m_cache.push_back(std::move(lr));
  }
  m_resultsQuery = set.query;
  if (m_onResultsChanged) {
    m_onResultsChanged();
  }
}
