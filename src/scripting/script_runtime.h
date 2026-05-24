#pragma once

#include "scripting/scripted_widget_types.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class ClipboardService;

namespace scripting {

  class ScriptRuntime {
  public:
    using SubscriberId = std::uint64_t;

    explicit ScriptRuntime(
        std::string runtimeName, ScriptWidgetSettings settings, ClipboardService* clipboard = nullptr
    );
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    [[nodiscard]] SubscriberId subscribe(ScriptWidgetResultCallback callback);
    void unsubscribe(SubscriberId id);
    void stop();

    void start(std::string chunkName, std::string source, ScriptWidgetSnapshot snapshot);
    void reload(std::string chunkName, std::string source, ScriptWidgetSnapshot snapshot);
    [[nodiscard]] bool enqueueUpdate(ScriptWidgetSnapshot snapshot);
    [[nodiscard]] bool enqueueCall(std::string functionName, ScriptWidgetSnapshot snapshot);
    [[nodiscard]] bool enqueueCallBool(std::string functionName, bool value, ScriptWidgetSnapshot snapshot);
    [[nodiscard]] bool
    enqueueCallStrings(std::string functionName, std::string first, std::string second, ScriptWidgetSnapshot snapshot);
    [[nodiscard]] bool enqueueAsyncCommandResult(std::uint64_t hostId, int callbackRef, process::RunResult result);
    [[nodiscard]] bool hasOnIpc() const;
    [[nodiscard]] bool unhealthy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };

  struct SharedScriptRuntimeAcquireResult {
    std::shared_ptr<ScriptRuntime> runtime;
    bool created = false;
  };

  class SharedScriptRuntimeRegistry {
  public:
    static SharedScriptRuntimeAcquireResult
    acquire(const std::string& key, ScriptWidgetSettings settings, ClipboardService* clipboard = nullptr);
  };

} // namespace scripting
