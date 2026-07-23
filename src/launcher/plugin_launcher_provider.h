#pragma once

#include "config/config_types.h"
#include "core/files/file_watcher.h"
#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"
#include "scripting/script_runtime.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class HttpClient;
class ClipboardService;
namespace scripting {
  struct PluginRuntimeContext;
  class ScriptApiContext;
} // namespace scripting

struct PluginLauncherProviderOptions {
  std::string displayName;
  std::string prefix;
  std::string glyph;
  bool globalSearch = false;
  int debounceMs = 0;
  std::vector<LauncherCategory> categories;
};

// A launcher provider backed by a plugin's [[launcher_provider]] entry. The native
// LauncherProvider interface is synchronous, but the plugin's onQuery(text) runs
// off-thread, so query() returns the cached result set immediately and kicks an
// onQuery on the runtime; when results land, the panel is asked to re-gather the
// current query. Stale results are dropped by the runtime's generation counters,
// and the result set echoes the query text it answered so the latest one wins.
class PluginLauncherProvider : public LauncherProvider {
public:
  PluginLauncherProvider(scripting::PluginRuntimeContext context, PluginLauncherProviderOptions options);
  ~PluginLauncherProvider() override;

  [[nodiscard]] std::string_view defaultPrefix() const override { return m_prefix; }
  [[nodiscard]] std::string_view id() const override { return m_entryId; }
  [[nodiscard]] std::string displayName() const override { return m_displayName.empty() ? m_entryId : m_displayName; }
  [[nodiscard]] std::string_view defaultGlyphName() const override {
    return m_glyph.empty() ? std::string_view("search") : std::string_view(m_glyph);
  }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return m_globalSearch; }
  [[nodiscard]] std::vector<LauncherCategory> categories() const override { return m_categories; }
  [[nodiscard]] bool isDynamic() const override { return true; }
  void setResultsChangedCallback(std::function<void()> callback) override { m_onResultsChanged = std::move(callback); }
  void setQueryRequestedCallback(std::function<void(std::string)> callback) override {
    m_onQueryRequested = std::move(callback);
  }
  void setActivationDoneCallback(std::function<void(std::string, bool)> callback) override {
    m_onActivationDone = std::move(callback);
  }

  void initialize() override;
  void reset() override;
  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

private:
  void setupScriptWatch();
  void teardownScriptWatch();
  void reloadScript();
  void handleResult(const scripting::ScriptResult& result);
  // Prepend this provider's resolved prefix to a plugin-supplied sub-query, so a
  // rewritten launcher input keeps the provider active regardless of the configured
  // prefix. An empty prefix (global provider) yields the sub-query verbatim.
  [[nodiscard]] std::string buildProviderInput(std::string_view sub) const;
  // Enqueue onQuery for `text` on the runtime (skips a duplicate of the last send).
  void dispatchQuery(const std::string& text) const;
  // Restart the debounce timer; it fires dispatchQuery(m_pendingQuery) when idle.
  void armQueryTimer() const;

  std::string m_entryId;
  std::string m_displayName;
  std::filesystem::path m_sourcePath;
  std::filesystem::path m_pluginDir;
  std::string m_prefix;
  std::string m_glyph;
  bool m_globalSearch = false;
  int m_debounceMs = 0;
  std::vector<LauncherCategory> m_categories;
  std::unordered_map<std::string, WidgetSettingValue> m_settings;
  scripting::ScriptApiContext& m_scriptApi;
  FileWatcher* m_fileWatcher = nullptr;
  HttpClient* m_httpClient = nullptr;
  ClipboardService* m_clipboard = nullptr;
  std::shared_ptr<scripting::ScriptRuntime> m_runtime;
  scripting::ScriptRuntime::SubscriberId m_subscription = 0;
  FileWatcher::WatchId m_watchId = 0;
  std::function<void()> m_onResultsChanged;
  std::function<void(std::string)> m_onQueryRequested;
  std::function<void(std::string, bool)> m_onActivationDone;

  // An onActivate call is in flight; the panel close waits for its result so the
  // handler can rewrite the query (keeping the panel open) instead of closing.
  bool m_pendingActivate = false;
  std::string m_pendingActivateId;

  // query() is const but maintains the async cache: the latest results, the query
  // they answer, and the query last dispatched (to avoid re-sending the same text).
  mutable std::vector<LauncherResult> m_cache;
  mutable std::string m_resultsQuery;
  mutable std::string m_lastSentQuery;
  mutable std::string m_pendingQuery;
  mutable bool m_hasSentInitial = false;
  mutable Timer m_queryTimer;

  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
