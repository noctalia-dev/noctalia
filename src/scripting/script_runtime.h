#pragma once

#include "scripting/script_runtime_types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class ClipboardService;
class HttpClient;

namespace scripting {

  class ScriptApiContext;

  // While a plugin-manager disable/uninstall mutation synchronously rebuilds entry hosts,
  // make their ordinary stop() calls report the explicit plugin lifecycle reason. Scopes
  // are nest-safe per plugin and affect only runtimes whose name starts with
  // "<plugin-id>:".
  class PluginExitReasonScope {
  public:
    PluginExitReasonScope(std::string_view pluginId, ScriptExitReason reason);
    ~PluginExitReasonScope();

    PluginExitReasonScope(const PluginExitReasonScope&) = delete;
    PluginExitReasonScope& operator=(const PluginExitReasonScope&) = delete;

  private:
    std::string m_pluginId;
    std::optional<ScriptExitReason> m_previous;
  };

  class ScriptRuntime {
  public:
    using SubscriberId = std::uint64_t;
    using TogglePanelCallback = std::function<void(std::string_view)>;

    explicit ScriptRuntime(
        std::string runtimeName, ScriptSettings settings, ScriptApiContext& api, std::filesystem::path pluginDir,
        HttpClient* httpClient = nullptr, ClipboardService* clipboard = nullptr,
        TogglePanelCallback togglePanelCallback = {}
    );
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    [[nodiscard]] SubscriberId subscribe(ScriptResultCallback callback);
    void unsubscribe(SubscriberId id);
    // Stop the runtime and pass its reason to onExit(signal, reason).
    void stop(ScriptExitReason exitReason = ScriptExitReason::Reload);

    // Records the process signal delivered during graceful shell shutdown so
    // entry onExit callbacks can distinguish SIGINT/SIGTERM from ordinary teardown.
    static void setShutdownSignal(int signal) noexcept;

    void start(std::string chunkName, std::string source, ScriptSnapshot snapshot);
    void reload(std::string chunkName, std::string source, ScriptSnapshot snapshot);
    [[nodiscard]] bool enqueueUpdate(ScriptSnapshot snapshot);
    [[nodiscard]] bool enqueueCall(std::string functionName, ScriptSnapshot snapshot);
    // Calls a global with an arbitrary argument list. Each ScriptArg arrives in
    // Luau as the value it holds (boolean, number, string), in order.
    [[nodiscard]] bool
    enqueueCallArgs(std::string functionName, ScriptArgs args, ScriptSnapshot snapshot, ScriptCallOptions options = {});
    [[nodiscard]] bool enqueueCallBool(std::string functionName, bool value, ScriptSnapshot snapshot);
    [[nodiscard]] bool enqueueCallStrings(
        std::string functionName, std::string first, std::string second, ScriptSnapshot snapshot, bool coalesce = false
    );
    [[nodiscard]] bool enqueueAsyncCommandResult(std::uint64_t hostId, int callbackRef, process::RunResult result);
    // Swap the live settings snapshot and, if the script defines a global
    // onConfigChanged, invoke it — without tearing down the runtime.
    [[nodiscard]] bool enqueueSettingsChanged(ScriptSettings newSettings, ScriptSnapshot snapshot = {});
    [[nodiscard]] bool hasOnIpc() const;
    // True once the script has loaded and defines a global onConfigChanged handler.
    [[nodiscard]] bool hasOnConfigChanged() const;
    // True once the script has loaded and defines a global onActivate handler. The
    // launcher uses this to decide whether activating a result must wait for the
    // handler (which may rewrite the query) before closing the panel.
    [[nodiscard]] bool hasOnActivate() const;
    // True once the script has loaded and defines a global onScroll handler. Bar
    // widgets use this to leave scroll events unconsumed when the plugin has no
    // handler, so they still reach the bar underneath.
    [[nodiscard]] bool hasOnScroll() const;
    // True once the script has loaded and defines a global onKey. A plugin panel checks this
    // before consuming a capture_keys chord, so declaring one without a handler does not
    // silently swallow the key.
    [[nodiscard]] bool hasOnKey() const;
    [[nodiscard]] bool unhealthy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };

} // namespace scripting
