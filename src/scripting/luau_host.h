#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct lua_State;
class CompositorPlatform;

namespace process {
  struct RunResult;
}
namespace scripting {
  struct ScriptedWidgetBindingContext;
}

class LuauHost {
public:
  explicit LuauHost(CompositorPlatform* platform = nullptr);
  ~LuauHost();

  LuauHost(const LuauHost&) = delete;
  LuauHost& operator=(const LuauHost&) = delete;

  using AsyncCommandResultHandler =
      std::function<void(std::uint64_t hostId, int callbackRef, process::RunResult result)>;
  using AsyncProcessMatchResultHandler = std::function<void(std::uint64_t hostId, int callbackRef, bool matched)>;

  // Compile and load `source` as a chunk named `chunkName`. The chunk is left
  // on the Lua stack as a callable; call run() to execute it.
  // Returns true on success; on failure the error is logged.
  bool loadString(std::string_view chunkName, std::string_view source);

  // Pop the chunk from loadString() and pcall it with no args / no results.
  bool run();

  // Convenience: loadString + run.
  bool exec(std::string_view chunkName, std::string_view source) { return loadString(chunkName, source) && run(); }

  bool callGlobal(const char* name);
  bool callGlobalWithBool(const char* name, bool value);
  bool callGlobalWithStrings(const char* name, std::string_view first, std::string_view second);
  bool hasGlobal(const char* name);
  std::optional<std::string> callGlobalReturningString(const char* name);
  bool callGlobalWithBudget(const char* name, std::chrono::milliseconds budget);
  bool callGlobalWithBoolAndBudget(const char* name, bool value, std::chrono::milliseconds budget);
  bool callGlobalWithStringsAndBudget(
      const char* name, std::string_view first, std::string_view second, std::chrono::milliseconds budget
  );
  bool callAsyncCommandCallback(int callbackRef, const process::RunResult& result, std::chrono::milliseconds budget);
  bool callAsyncProcessMatchCallback(int callbackRef, bool matched, std::chrono::milliseconds budget);
  [[nodiscard]] bool lastCallTimedOut() const noexcept { return m_lastCallTimedOut; }

  lua_State* state() { return m_T; }
  [[nodiscard]] CompositorPlatform* platform() const noexcept { return m_platform; }
  [[nodiscard]] std::uint64_t hostId() const noexcept { return m_hostId; }
  void setScriptContext(scripting::ScriptedWidgetBindingContext* context) { m_scriptContext = context; }
  void setMuteErrors(bool mute) { m_muteErrors = mute; }
  void setAsyncCommandResultHandler(AsyncCommandResultHandler handler) {
    m_asyncCommandResultHandler = std::move(handler);
  }
  void setAsyncProcessMatchResultHandler(AsyncProcessMatchResultHandler handler) {
    m_asyncProcessMatchResultHandler = std::move(handler);
  }
  [[nodiscard]] bool startAsyncCommand(std::string command, int callbackRef, std::chrono::milliseconds timeout);
  [[nodiscard]] bool startAsyncProcessMatch(std::vector<std::string> needles, int callbackRef);
  [[nodiscard]] bool hasAsyncCommandCallback(int callbackRef) const;
  [[nodiscard]] bool hasAsyncProcessMatchCallback(int callbackRef) const;
  void interruptIfBudgetExceeded(lua_State* L);
  void scriptLog(std::string message);
  void scriptNotifyInfo(std::string title, std::string body);
  void scriptNotifyError(std::string title, std::string body);
  [[nodiscard]] bool scriptCopyToClipboard(std::string text, std::string mimeType);
  [[nodiscard]] std::optional<std::string> scriptFocusedOutputName() const;

private:
  bool callGlobalInternal(const char* name, int args, std::chrono::milliseconds budget);
  bool callWithBudget(const char* name, int args, int results, std::chrono::milliseconds budget);
  void beginBudget(std::string_view name, std::chrono::milliseconds budget);
  void endBudget();

  std::uint64_t m_hostId = 0;
  CompositorPlatform* m_platform = nullptr;
  scripting::ScriptedWidgetBindingContext* m_scriptContext = nullptr;
  lua_State* m_L = nullptr; // main state, frozen by luaL_sandbox
  lua_State* m_T = nullptr; // sandboxed thread; user code runs here
  int m_threadRef = -1;     // registry ref pinning m_T against the GC
  std::unordered_set<int> m_asyncCommandCallbackRefs;
  std::unordered_set<int> m_asyncProcessMatchCallbackRefs;
  AsyncCommandResultHandler m_asyncCommandResultHandler;
  AsyncProcessMatchResultHandler m_asyncProcessMatchResultHandler;
  std::chrono::steady_clock::time_point m_callDeadline{};
  std::string m_currentCallName;
  bool m_budgetActive = false;
  bool m_lastCallTimedOut = false;
  bool m_muteErrors = false;
};
