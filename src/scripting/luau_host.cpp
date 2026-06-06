#include "scripting/luau_host.h"

#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "core/process.h"
#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "notification/notifications.h"
#include "scripting/script_api_context.h"
#include "scripting/scripted_widget_bindings.h"
#include "scripting/scripted_widget_manifest.h"
#include "system/terminal_launch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
  Logger kLog{"luau"};
  constexpr const char* kHostKey = "__noctalia_host";
  constexpr auto kDefaultCommandTimeout = std::chrono::milliseconds(5000);
  constexpr auto kMinCommandTimeout = std::chrono::milliseconds(50);
  constexpr auto kMaxCommandTimeout = std::chrono::milliseconds(60000);
  constexpr std::size_t kMaxAsyncCommandOutputBytes = 1024 * 1024;
  constexpr std::size_t kMaxAsyncCommandsPerHost = 8;
  constexpr int kMaxGlobalAsyncCommands = 32;
  constexpr std::size_t kMaxAsyncProcessMatchesPerHost = 16;
  constexpr int kMaxGlobalAsyncProcessMatches = 64;
  constexpr int kMaxGlobalDetachedCommands = 32;

  std::uint64_t& nextHostId() {
    static std::uint64_t id = 1;
    return id;
  }

  std::atomic<int>& inFlightAsyncCommands() {
    static std::atomic<int> count{0};
    return count;
  }

  std::atomic<int>& inFlightAsyncProcessMatches() {
    static std::atomic<int> count{0};
    return count;
  }

  std::atomic<int>& inFlightDetachedCommands() {
    static std::atomic<int> count{0};
    return count;
  }

  bool acquireDetachedCommandSlot() {
    auto& globalInFlight = inFlightDetachedCommands();
    int current = globalInFlight.load(std::memory_order_relaxed);
    while (current < kMaxGlobalDetachedCommands) {
      if (globalInFlight.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  void releaseDetachedCommandSlot() { inFlightDetachedCommands().fetch_sub(1, std::memory_order_relaxed); }

  bool startDetachedCommandAsync(std::string command) {
    if (command.empty()) {
      return false;
    }
    if (!acquireDetachedCommandSlot()) {
      return false;
    }

    try {
      std::thread([command = std::move(command)]() mutable {
        try {
          (void)process::runAsync(command);
        } catch (...) {
        }
        releaseDetachedCommandSlot();
      }).detach();
    } catch (...) {
      releaseDetachedCommandSlot();
      return false;
    }

    return true;
  }

  bool startDetachedProcessAsync(std::vector<std::string> args) {
    if (args.empty() || args.front().empty()) {
      return false;
    }
    if (!acquireDetachedCommandSlot()) {
      return false;
    }

    try {
      std::thread([args = std::move(args)]() mutable {
        try {
          (void)process::runAsync(args);
        } catch (...) {
        }
        releaseDetachedCommandSlot();
      }).detach();
    } catch (...) {
      releaseDetachedCommandSlot();
      return false;
    }

    return true;
  }

  bool startDetachedCommandInTerminalAsync(std::string command) {
    auto prepared = terminal_launch::prepareCommand(command);
    return prepared.has_value() && startDetachedProcessAsync(std::move(*prepared));
  }

  std::chrono::milliseconds commandTimeoutFromLua(lua_State* L) {
    const double rawTimeout = luaL_optnumber(
        L, 3, static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultCommandTimeout).count())
    );
    const double timeoutMs =
        std::isfinite(rawTimeout) ? rawTimeout : static_cast<double>(kDefaultCommandTimeout.count());
    const double bounded = std::clamp(
        timeoutMs, static_cast<double>(kMinCommandTimeout.count()), static_cast<double>(kMaxCommandTimeout.count())
    );
    return std::chrono::milliseconds(static_cast<int>(bounded));
  }

  void budgetInterrupt(lua_State* L, int /*gc*/) {
    auto* host = static_cast<LuauHost*>(lua_callbacks(L)->userdata);
    if (host != nullptr) {
      host->interruptIfBudgetExceeded(L);
    }
  }

  void setTableInteger(lua_State* L, const char* key, int value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
  }

  void setTableString(lua_State* L, const char* key, const std::string& value) {
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, key);
  }

  void setTableBool(lua_State* L, const char* key, bool value) {
    lua_pushboolean(L, value ? 1 : 0);
    lua_setfield(L, -2, key);
  }

  LuauHost* hostForState(lua_State* L) {
    lua_getglobal(L, kHostKey);
    auto* host = static_cast<LuauHost*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return host;
  }

  int luau_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    if (auto* host = hostForState(L)) {
      host->scriptLog(msg);
    } else {
      kLog.info("{}", msg);
    }
    return 0;
  }

  int luau_runAsync(lua_State* L) {
    size_t len = 0;
    const char* cmd = luaL_checklstring(L, 1, &len);
    std::string command(cmd, len);

    if (lua_isnoneornil(L, 2)) {
      bool ok = startDetachedCommandAsync(std::move(command));
      lua_pushboolean(L, ok ? 1 : 0);
      return 1;
    }

    luaL_checktype(L, 2, LUA_TFUNCTION);

    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }

    const auto timeout = commandTimeoutFromLua(L);
    const int callbackRef = lua_ref(L, 2);
    bool ok = host->startAsyncCommand(std::move(command), callbackRef, timeout);
    if (!ok) {
      lua_unref(L, callbackRef);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_runInTerminal(lua_State* L) {
    size_t len = 0;
    const char* cmd = luaL_checklstring(L, 1, &len);
    bool ok = startDetachedCommandInTerminalAsync(std::string(cmd, len));
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_commandExists(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, process::commandExists(name) ? 1 : 0);
    return 1;
  }

  int luau_flatpakAppInstalled(lua_State* L) {
    size_t len = 0;
    const char* appId = luaL_checklstring(L, 1, &len);
    lua_pushboolean(L, process::flatpakAppInstalled(std::string_view(appId, len)) ? 1 : 0);
    return 1;
  }

  int luau_portalAvailable(lua_State* L) {
    lua_pushboolean(L, process::desktopPortalAvailable() ? 1 : 0);
    return 1;
  }

  int luau_focusedOutputName(lua_State* L) {
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      return 1;
    }

    if (auto name = host->scriptFocusedOutputName(); name.has_value() && !name->empty()) {
      lua_pushlstring(L, name->data(), name->size());
      return 1;
    }

    if (host->platform() == nullptr) {
      lua_pushnil(L);
      return 1;
    }

    wl_output* output = host->platform()->preferredInteractiveOutput();
    const auto* info = host->platform()->findOutputByWl(output);
    if (info == nullptr || info->connectorName.empty()) {
      lua_pushnil(L);
      return 1;
    }

    lua_pushlstring(L, info->connectorName.data(), info->connectorName.size());
    return 1;
  }

  int luau_isDarkMode(lua_State* L) {
    auto* host = hostForState(L);
    lua_pushboolean(L, host != nullptr && host->api().isDarkMode() ? 1 : 0);
    return 1;
  }

  int luau_processMatches(lua_State* L) {
    const int count = lua_gettop(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);

    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }

    std::vector<std::string> needles;
    needles.reserve(static_cast<std::size_t>(std::max(0, count - 1)));
    for (int i = 2; i <= count; ++i) {
      size_t len = 0;
      const char* needle = luaL_checklstring(L, i, &len);
      needles.emplace_back(needle, len);
    }

    const int callbackRef = lua_ref(L, 1);
    bool ok = host->startAsyncProcessMatch(std::move(needles), callbackRef);
    if (!ok) {
      lua_unref(L, callbackRef);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_notify(lua_State* L) {
    const char* title = luaL_checkstring(L, 1);
    const char* body = luaL_optstring(L, 2, "");
    if (auto* host = hostForState(L)) {
      host->scriptNotifyInfo(title, body);
    } else {
      notify::info("Noctalia", title, body);
    }
    return 0;
  }

  int luau_notifyError(lua_State* L) {
    const char* title = luaL_checkstring(L, 1);
    const char* body = luaL_optstring(L, 2, "");
    if (auto* host = hostForState(L)) {
      host->scriptNotifyError(title, body);
    } else {
      notify::error("Noctalia", title, body);
    }
    return 0;
  }

  int luau_copyToClipboard(lua_State* L) {
    size_t textLen = 0;
    const char* text = luaL_checklstring(L, 1, &textLen);
    size_t mimeLen = 0;
    const char* mimeType = luaL_checklstring(L, 2, &mimeLen);

    bool ok = textLen > 0 && mimeLen > 0;
    if (ok) {
      if (auto* host = hostForState(L)) {
        ok = host->scriptCopyToClipboard(std::string(text, textLen), std::string(mimeType, mimeLen));
      } else {
        ok = false;
      }
    }

    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_getenv(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* val = std::getenv(name);
    if (val)
      lua_pushstring(L, val);
    else
      lua_pushnil(L);
    return 1;
  }

  int luau_expandPath(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    const std::string expanded = scripting::expandUserPath(std::string_view(path, len));
    lua_pushlstring(L, expanded.data(), expanded.size());
    return 1;
  }

  const luaL_Reg kNoctaliaBaseLib[] = {
      {"log", luau_log},
      {"runAsync", luau_runAsync},
      {"runInTerminal", luau_runInTerminal},
      {"commandExists", luau_commandExists},
      {"processMatches", luau_processMatches},
      {"flatpakAppInstalled", luau_flatpakAppInstalled},
      {"portalAvailable", luau_portalAvailable},
      {"focusedOutputName", luau_focusedOutputName},
      {"isDarkMode", luau_isDarkMode},
      {"notify", luau_notify},
      {"notifyError", luau_notifyError},
      {"copyToClipboard", luau_copyToClipboard},
      {"getenv", luau_getenv},
      {"expandPath", luau_expandPath},
      {nullptr, nullptr},
  };

  void registerNoctaliaLib(lua_State* L) {
    luaL_register(L, "noctalia", kNoctaliaBaseLib);
    lua_pop(L, 1);
  }
} // namespace

LuauHost::LuauHost(scripting::ScriptApiContext& api, CompositorPlatform* platform) : m_api(api), m_platform(platform) {
  m_hostId = nextHostId()++;

  m_L = luaL_newstate();
  lua_callbacks(m_L)->userdata = this;
  lua_callbacks(m_L)->interrupt = budgetInterrupt;
  luaL_openlibs(m_L);
  registerNoctaliaLib(m_L);
  // Freeze main state's stdlib + globals. The thread we create next inherits
  // reads from this frozen table but gets its own writable globals, so the
  // user script can define `function update()` without touching the parent.
  luaL_sandbox(m_L);

  m_T = lua_newthread(m_L);
  luaL_sandboxthread(m_T);
  lua_pushlightuserdata(m_T, this);
  lua_setglobal(m_T, kHostKey);
  // lua_newthread leaves the thread on the main stack; pin it in the registry
  // so the GC can't collect it, then drop the stack reference.
  m_threadRef = lua_ref(m_L, -1);
  lua_pop(m_L, 1);
}

LuauHost::~LuauHost() {
  if (m_L) {
    if (m_T != nullptr) {
      for (int callbackRef : m_asyncCommandCallbackRefs) {
        lua_unref(m_T, callbackRef);
      }
      m_asyncCommandCallbackRefs.clear();
      for (int callbackRef : m_asyncProcessMatchCallbackRefs) {
        lua_unref(m_T, callbackRef);
      }
      m_asyncProcessMatchCallbackRefs.clear();
    }
    if (m_threadRef != -1)
      lua_unref(m_L, m_threadRef);
    lua_close(m_L);
  }
}

bool LuauHost::startAsyncCommand(std::string command, int callbackRef, std::chrono::milliseconds timeout) {
  if (command.empty() || callbackRef <= LUA_REFNIL || m_asyncCommandCallbackRefs.size() >= kMaxAsyncCommandsPerHost) {
    return false;
  }

  auto& globalInFlight = inFlightAsyncCommands();
  int current = globalInFlight.load(std::memory_order_relaxed);
  while (current < kMaxGlobalAsyncCommands) {
    if (globalInFlight.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
      break;
    }
  }
  if (current >= kMaxGlobalAsyncCommands) {
    return false;
  }

  m_asyncCommandCallbackRefs.insert(callbackRef);
  auto handler = m_asyncCommandResultHandler;
  if (!handler) {
    m_asyncCommandCallbackRefs.erase(callbackRef);
    globalInFlight.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }
  try {
    std::thread([hostId = m_hostId, callbackRef, command = std::move(command), timeout,
                 handler = std::move(handler)]() mutable {
      auto result =
          process::runSyncWithTimeoutAndOutputLimit({"/bin/sh", "-c", command}, timeout, kMaxAsyncCommandOutputBytes);
      inFlightAsyncCommands().fetch_sub(1, std::memory_order_relaxed);
      handler(hostId, callbackRef, std::move(result));
    }).detach();
  } catch (...) {
    m_asyncCommandCallbackRefs.erase(callbackRef);
    globalInFlight.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  return true;
}

bool LuauHost::startAsyncProcessMatch(std::vector<std::string> needles, int callbackRef) {
  if (needles.empty()
      || callbackRef <= LUA_REFNIL
      || m_asyncProcessMatchCallbackRefs.size() >= kMaxAsyncProcessMatchesPerHost) {
    return false;
  }

  if (std::any_of(needles.begin(), needles.end(), [](const auto& needle) { return needle.empty(); })) {
    return false;
  }

  auto& globalInFlight = inFlightAsyncProcessMatches();
  int current = globalInFlight.load(std::memory_order_relaxed);
  while (current < kMaxGlobalAsyncProcessMatches) {
    if (globalInFlight.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
      break;
    }
  }
  if (current >= kMaxGlobalAsyncProcessMatches) {
    return false;
  }

  m_asyncProcessMatchCallbackRefs.insert(callbackRef);
  auto handler = m_asyncProcessMatchResultHandler;
  if (!handler) {
    m_asyncProcessMatchCallbackRefs.erase(callbackRef);
    globalInFlight.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  try {
    std::thread([hostId = m_hostId, callbackRef, needles = std::move(needles), handler = std::move(handler)]() mutable {
      bool matched = false;
      try {
        matched = process::commandLineMatchesAll(needles);
      } catch (...) {
      }
      inFlightAsyncProcessMatches().fetch_sub(1, std::memory_order_relaxed);
      handler(hostId, callbackRef, matched);
    }).detach();
  } catch (...) {
    m_asyncProcessMatchCallbackRefs.erase(callbackRef);
    globalInFlight.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  return true;
}

bool LuauHost::hasAsyncCommandCallback(int callbackRef) const {
  return m_asyncCommandCallbackRefs.contains(callbackRef);
}

bool LuauHost::hasAsyncProcessMatchCallback(int callbackRef) const {
  return m_asyncProcessMatchCallbackRefs.contains(callbackRef);
}

bool LuauHost::callAsyncCommandCallback(
    int callbackRef, const process::RunResult& result, std::chrono::milliseconds budget
) {
  if (m_T == nullptr) {
    return false;
  }
  const auto it = m_asyncCommandCallbackRefs.find(callbackRef);
  if (it == m_asyncCommandCallbackRefs.end()) {
    return false;
  }
  m_asyncCommandCallbackRefs.erase(it);

  lua_getref(m_T, callbackRef);
  lua_unref(m_T, callbackRef);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }

  lua_createtable(m_T, 0, 6);
  setTableInteger(m_T, "exitCode", result.exitCode);
  setTableString(m_T, "stdout", result.out);
  setTableString(m_T, "stderr", result.err);
  setTableBool(m_T, "timedOut", result.timedOut);
  setTableBool(m_T, "stdoutTruncated", result.outTruncated);
  setTableBool(m_T, "stderrTruncated", result.errTruncated);

  return callWithBudget("async command callback", 1, 0, budget);
}

bool LuauHost::callAsyncProcessMatchCallback(int callbackRef, bool matched, std::chrono::milliseconds budget) {
  if (m_T == nullptr) {
    return false;
  }
  const auto it = m_asyncProcessMatchCallbackRefs.find(callbackRef);
  if (it == m_asyncProcessMatchCallbackRefs.end()) {
    return false;
  }
  m_asyncProcessMatchCallbackRefs.erase(it);

  lua_getref(m_T, callbackRef);
  lua_unref(m_T, callbackRef);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }

  lua_pushboolean(m_T, matched ? 1 : 0);
  return callWithBudget("process match callback", 1, 0, budget);
}

void LuauHost::interruptIfBudgetExceeded(lua_State* L) {
  if (!m_budgetActive) {
    return;
  }
  if (std::chrono::steady_clock::now() <= m_callDeadline) {
    return;
  }
  m_lastCallTimedOut = true;
  m_budgetActive = false;
  luaL_error(L, "script callback '%s' timed out", m_currentCallName.empty() ? "(unknown)" : m_currentCallName.c_str());
}

void LuauHost::scriptLog(std::string message) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->sideEffects.push_back(
        {.kind = scripting::ScriptWidgetSideEffectKind::Log, .title = std::move(message), .body = {}}
    );
    return;
  }
  kLog.info("{}", message);
}

void LuauHost::scriptNotifyInfo(std::string title, std::string body) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->sideEffects.push_back(
        {.kind = scripting::ScriptWidgetSideEffectKind::NotifyInfo, .title = std::move(title), .body = std::move(body)}
    );
    return;
  }
  notify::info("Noctalia", title, body);
}

void LuauHost::scriptNotifyError(std::string title, std::string body) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->sideEffects.push_back(
        {.kind = scripting::ScriptWidgetSideEffectKind::NotifyError, .title = std::move(title), .body = std::move(body)}
    );
    return;
  }
  notify::error("Noctalia", title, body);
}

bool LuauHost::scriptCopyToClipboard(std::string text, std::string mimeType) {
  if (m_scriptContext == nullptr || text.empty() || mimeType.empty()) {
    return false;
  }
  m_scriptContext->sideEffects.push_back(
      {.kind = scripting::ScriptWidgetSideEffectKind::CopyToClipboard,
       .title = std::move(text),
       .body = std::move(mimeType)}
  );
  return true;
}

std::optional<std::string> LuauHost::scriptFocusedOutputName() const {
  if (m_scriptContext == nullptr || m_scriptContext->snapshot.focusedOutputName.empty()) {
    return std::nullopt;
  }
  return m_scriptContext->snapshot.focusedOutputName;
}

void LuauHost::beginBudget(std::string_view name, std::chrono::milliseconds budget) {
  m_currentCallName = std::string(name);
  m_callDeadline = std::chrono::steady_clock::now() + std::max(budget, std::chrono::milliseconds(1));
  m_lastCallTimedOut = false;
  m_budgetActive = true;
}

void LuauHost::endBudget() { m_budgetActive = false; }

bool LuauHost::callWithBudget(const char* name, int args, int results, std::chrono::milliseconds budget) {
  beginBudget(name != nullptr ? name : "(unknown)", budget);
  int rc = lua_pcall(m_T, args, results, 0);
  endBudget();
  if (rc != 0) {
    const char* err = lua_tostring(m_T, -1);
    if (!m_muteErrors) {
      kLog.error("call to '{}' failed: {}", name ? name : "(unknown)", err ? err : "(no error)");
    }
    lua_pop(m_T, 1);
    return false;
  }
  return true;
}

bool LuauHost::callGlobalInternal(const char* name, int args, std::chrono::milliseconds budget) {
  return callWithBudget(name, args, 0, budget);
}

bool LuauHost::loadString(std::string_view chunkName, std::string_view source) {
  size_t bytecodeSize = 0;
  char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
  if (!bytecode) {
    kLog.error("luau_compile returned null for chunk '{}'", std::string(chunkName));
    return false;
  }
  std::string name(chunkName);
  int loadResult = luau_load(m_T, name.c_str(), bytecode, bytecodeSize, 0);
  std::free(bytecode);
  if (loadResult != 0) {
    const char* err = lua_tostring(m_T, -1);
    kLog.error("luau_load failed for '{}': {}", name, err ? err : "(no error)");
    lua_pop(m_T, 1);
    return false;
  }
  return true;
}

bool LuauHost::run() { return callWithBudget("chunk", 0, 0, std::chrono::milliseconds(100)); }

bool LuauHost::hasGlobal(const char* name) {
  lua_getglobal(m_T, name);
  bool exists = lua_isfunction(m_T, -1);
  lua_pop(m_T, 1);
  return exists;
}

bool LuauHost::callGlobal(const char* name) { return callGlobalWithBudget(name, std::chrono::milliseconds(25)); }

bool LuauHost::callGlobalWithBudget(const char* name, std::chrono::milliseconds budget) {
  lua_getglobal(m_T, name);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }
  return callGlobalInternal(name, 0, budget);
}

bool LuauHost::callGlobalWithBool(const char* name, bool value) {
  return callGlobalWithBoolAndBudget(name, value, std::chrono::milliseconds(25));
}

bool LuauHost::callGlobalWithBoolAndBudget(const char* name, bool value, std::chrono::milliseconds budget) {
  lua_getglobal(m_T, name);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }
  lua_pushboolean(m_T, value ? 1 : 0);
  return callGlobalInternal(name, 1, budget);
}

bool LuauHost::callGlobalWithStrings(const char* name, std::string_view first, std::string_view second) {
  return callGlobalWithStringsAndBudget(name, first, second, std::chrono::milliseconds(25));
}

bool LuauHost::callGlobalWithStringsAndBudget(
    const char* name, std::string_view first, std::string_view second, std::chrono::milliseconds budget
) {
  lua_getglobal(m_T, name);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }
  lua_pushlstring(m_T, first.data(), first.size());
  lua_pushlstring(m_T, second.data(), second.size());
  return callGlobalInternal(name, 2, budget);
}

std::optional<std::string> LuauHost::callGlobalReturningString(const char* name) {
  lua_getglobal(m_T, name);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return std::nullopt;
  }
  int rc = lua_pcall(m_T, 0, 1, 0);
  if (rc != 0) {
    const char* err = lua_tostring(m_T, -1);
    kLog.error("call to '{}' failed: {}", name, err ? err : "(no error)");
    lua_pop(m_T, 1);
    return std::nullopt;
  }
  std::optional<std::string> result;
  if (lua_isstring(m_T, -1)) {
    size_t len = 0;
    const char* s = lua_tolstring(m_T, -1, &len);
    result = std::string(s, len);
  }
  lua_pop(m_T, 1);
  return result;
}
