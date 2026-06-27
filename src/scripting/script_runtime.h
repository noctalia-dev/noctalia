#pragma once

#include "scripting/script_runtime_types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

class ClipboardService;
class HttpClient;

namespace scripting {

  class ScriptApiContext;

  class ScriptRuntime {
  public:
    using SubscriberId = std::uint64_t;

    explicit ScriptRuntime(
        std::string runtimeName, ScriptSettings settings, ScriptApiContext& api, std::filesystem::path pluginDir,
        HttpClient* httpClient = nullptr, ClipboardService* clipboard = nullptr
    );
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    [[nodiscard]] SubscriberId subscribe(ScriptResultCallback callback);
    void unsubscribe(SubscriberId id);
    void stop();

    void start(std::string chunkName, std::string source, ScriptSnapshot snapshot);
    void reload(std::string chunkName, std::string source, ScriptSnapshot snapshot);
    [[nodiscard]] bool enqueueUpdate(ScriptSnapshot snapshot);
    [[nodiscard]] bool enqueueCall(std::string functionName, ScriptSnapshot snapshot);
    [[nodiscard]] bool enqueueCallBool(std::string functionName, bool value, ScriptSnapshot snapshot);
    [[nodiscard]] bool enqueueCallStrings(
        std::string functionName, std::string first, std::string second, ScriptSnapshot snapshot, bool coalesce = false
    );
    [[nodiscard]] bool enqueueAsyncCommandResult(std::uint64_t hostId, int callbackRef, process::RunResult result);
    [[nodiscard]] bool hasOnIpc() const;
    // True once the script has loaded and defines a global onActivate handler. The
    // launcher uses this to decide whether activating a result must wait for the
    // handler (which may rewrite the query) before closing the panel.
    [[nodiscard]] bool hasOnActivate() const;
    [[nodiscard]] bool unhealthy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };

} // namespace scripting
