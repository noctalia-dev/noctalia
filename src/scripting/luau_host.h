#pragma once

#include "scripting/plugin_i18n.h"
#include "scripting/script_arg.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;
class CompositorPlatform;
class HttpClient;
struct Color;
struct HttpRequest;

namespace process {
  struct RunResult;
}
namespace scripting {
  class ScriptApiContext;
  struct PluginBindingContext;
} // namespace scripting

class LuauHost {
public:
  explicit LuauHost(scripting::ScriptApiContext& api, CompositorPlatform* platform = nullptr);
  ~LuauHost();

  LuauHost(const LuauHost&) = delete;
  LuauHost& operator=(const LuauHost&) = delete;

  using AsyncCommandResultHandler =
      std::function<void(std::uint64_t hostId, int callbackRef, process::RunResult result)>;
  using AsyncProcessMatchResultHandler = std::function<void(std::uint64_t hostId, int callbackRef, bool matched)>;
  using AsyncHttpResultHandler = std::function<
      void(std::uint64_t hostId, int callbackRef, bool ok, int status, std::string body, bool isDownload)>;
  using ColorPickerResultHandler =
      std::function<void(std::uint64_t hostId, int callbackRef, std::optional<std::string> color)>;
  // Registers a `noctalia.state.watch` callback with the shared store (the runtime
  // owns the token + delivery, so registration is delegated back to it).
  using StateWatchHandler = std::function<void(std::string key, int callbackRef)>;

  // Compile and load `source` as a chunk named `chunkName`. The chunk is left
  // on the Lua stack as a callable; call run() to execute it.
  // Returns true on success; on failure the error is logged.
  bool loadString(std::string_view chunkName, std::string_view source);

  // Pop the chunk from loadString() and pcall it with no args / no results.
  bool run();

  // Convenience: loadString + run.
  bool exec(std::string_view chunkName, std::string_view source) { return loadString(chunkName, source) && run(); }

  bool callGlobal(const char* name);
  bool hasGlobal(const char* name);
  std::optional<std::string> callGlobalReturningString(const char* name);
  bool callGlobalWithBudget(const char* name, std::chrono::milliseconds budget);
  // Calls a global with an arbitrary argument list; each ScriptArg is pushed as
  // the Luau value it holds, in order.
  bool callGlobalWithArgsAndBudget(
      const char* name, std::span<const scripting::ScriptArg> args, std::chrono::milliseconds budget
  );
  bool callAsyncCommandCallback(int callbackRef, const process::RunResult& result, std::chrono::milliseconds budget);
  bool callAsyncProcessMatchCallback(int callbackRef, bool matched, std::chrono::milliseconds budget);
  [[nodiscard]] bool lastCallTimedOut() const noexcept { return m_lastCallTimedOut; }

  lua_State* state() { return m_T; }
  [[nodiscard]] CompositorPlatform* platform() const noexcept { return m_platform; }
  [[nodiscard]] scripting::ScriptApiContext& api() const noexcept { return m_api; }
  [[nodiscard]] std::uint64_t hostId() const noexcept { return m_hostId; }
  void setScriptContext(scripting::PluginBindingContext* context) { m_scriptContext = context; }
  void setMuteErrors(bool mute) { m_muteErrors = mute; }
  // The plugin's own directory: relative filesystem/translation paths resolve against it.
  void setPluginDir(std::filesystem::path dir) { m_pluginDir = std::move(dir); }
  [[nodiscard]] const std::filesystem::path& pluginDir() const noexcept { return m_pluginDir; }
  // The owning plugin id ("author/plugin"): scopes the shared state store.
  void setPluginId(std::string id) { m_pluginId = std::move(id); }
  [[nodiscard]] const std::string& pluginId() const noexcept { return m_pluginId; }
  void setStateWatchHandler(StateWatchHandler handler) { m_stateWatchHandler = std::move(handler); }

  // noctalia.state.* — host-mediated per-plugin shared data.
  void stateSet(const std::string& key, std::string json);
  [[nodiscard]] std::optional<std::string> stateGet(const std::string& key) const;
  void stateWatch(std::string key, int callbackRef);
  bool callStateWatchCallback(int callbackRef, const std::string& json, std::chrono::milliseconds budget);
  [[nodiscard]] bool hasStateWatchCallback(int callbackRef) const;

  // noctalia.runStream — run a long-lived process and deliver each stdout line to a
  // Lua callback. Cancellable: every active stream's process is terminated when the
  // host is destroyed (reload / runtime stop), so editing the script or removing the
  // widget kills the subprocess instead of leaking it.
  using StreamLineHandler = std::function<void(std::uint64_t hostId, int callbackRef, std::string line)>;
  void setStreamLineHandler(StreamLineHandler handler) { m_streamLineHandler = std::move(handler); }
  [[nodiscard]] bool startStream(std::string command, int callbackRef);
  bool callStreamCallback(int callbackRef, const std::string& line, std::chrono::milliseconds budget);
  [[nodiscard]] bool hasStreamCallback(int callbackRef) const;

  // noctalia.httpStream — long-lived streaming HTTP request through the main-thread
  // HttpClient. Each received line goes to the line callback; the close callback fires
  // exactly once when the transfer ends (unless the stream was stopped). Streams are
  // cancelled on host destruction. `streamKey` identifies the stream (the line ref).
  using HttpStreamEventHandler =
      std::function<void(std::uint64_t hostId, int streamKey, bool closed, std::string line, bool ok, int status)>;
  void setHttpStreamEventHandler(HttpStreamEventHandler handler) { m_httpStreamEventHandler = std::move(handler); }
  // Returns the stream key (> 0) on success, 0 on failure (caller keeps ref ownership on failure).
  [[nodiscard]] int startHttpStream(HttpRequest request, int lineRef, int closeRef);
  void stopHttpStream(int streamKey);
  bool callHttpStreamLineCallback(int streamKey, const std::string& line, std::chrono::milliseconds budget);
  bool callHttpStreamCloseCallback(int streamKey, bool ok, int status, std::chrono::milliseconds budget);
  [[nodiscard]] bool hasHttpStream(int streamKey) const;

  // Load the plugin's own translations/<lang>.json (over en.json) into a flat dotted-key
  // catalog. Call after setPluginDir().
  void loadTranslations();
  // Resolve `key` in the plugin catalog and interpolate {name} placeholders from `subst`.
  // A missing key is logged and returned verbatim (no silent fallback chain).
  [[nodiscard]] std::string
  translate(std::string_view key, const std::unordered_map<std::string, std::string>& subst) const;
  [[nodiscard]] bool hasTranslation(std::string_view key) const { return m_translations.has(key); }
  void setAsyncCommandResultHandler(AsyncCommandResultHandler handler) {
    m_asyncCommandResultHandler = std::move(handler);
  }
  void setAsyncProcessMatchResultHandler(AsyncProcessMatchResultHandler handler) {
    m_asyncProcessMatchResultHandler = std::move(handler);
  }
  void setHttpClient(HttpClient* client) { m_httpClient = client; }
  void setAsyncHttpResultHandler(AsyncHttpResultHandler handler) { m_asyncHttpResultHandler = std::move(handler); }
  void setColorPickerResultHandler(ColorPickerResultHandler handler) {
    m_colorPickerResultHandler = std::move(handler);
  }
  [[nodiscard]] bool startAsyncCommand(std::string command, int callbackRef, std::chrono::milliseconds timeout);
  [[nodiscard]] bool startAsyncProcessMatch(std::vector<std::string> needles, int callbackRef);
  // HTTP/download dispatch to the main-thread HttpClient; the response is delivered back as an
  // AsyncHttpResult event. `isDownload` selects the on_done(bool) vs on_response(table) callback shape.
  [[nodiscard]] bool startAsyncHttp(HttpRequest request, int callbackRef);
  [[nodiscard]] bool startAsyncDownload(std::string url, std::string destPath, int callbackRef);
  bool callAsyncHttpCallback(
      int callbackRef, bool ok, int status, const std::string& body, std::chrono::milliseconds budget
  );
  bool callAsyncDownloadCallback(int callbackRef, bool ok, std::chrono::milliseconds budget);
  [[nodiscard]] bool startColorPicker(const Color& initialColor, int callbackRef);
  bool
  callColorPickerCallback(int callbackRef, const std::optional<std::string>& color, std::chrono::milliseconds budget);
  [[nodiscard]] bool hasAsyncCommandCallback(int callbackRef) const;
  [[nodiscard]] bool hasAsyncProcessMatchCallback(int callbackRef) const;
  [[nodiscard]] bool hasAsyncHttpCallback(int callbackRef) const;
  [[nodiscard]] bool hasColorPickerCallback(int callbackRef) const;
  void interruptIfBudgetExceeded(lua_State* L);
  void scriptLog(std::string message);
  // Request the runtime tick rate (how often update() fires). A runtime concern, so
  // it lives on noctalia.* and works for every entry type, including headless services.
  void scriptSetUpdateInterval(int ms);
  void scriptNotifyInfo(std::string title, std::string body);
  void scriptNotifyError(std::string title, std::string body);
  // Toggle the host wallpaper surface on an output. Queued as a side effect and
  // applied on the main thread (Wallpaper is not worker-thread safe).
  void scriptSetWallpaperEnabled(std::string connector, bool enabled);
  // Apply and persist a wallpaper image. Empty connector targets all outputs.
  // Queued as a side effect and applied on the main thread.
  void scriptSetWallpaper(std::string connector, std::string path);
  // Toggle a host panel by id ("author/plugin:panel"). Queued, applied on the main thread.
  void scriptTogglePanel(std::string panelId);
  [[nodiscard]] bool scriptCopyToClipboard(std::string text, std::string mimeType);
  [[nodiscard]] std::optional<std::string> scriptFocusedOutputName() const;

  // Bytes currently allocated by this VM (tracked by the custom allocator).
  [[nodiscard]] std::size_t memoryUsedBytes() const noexcept { return m_memUsed; }

private:
  // lua_Alloc for this VM: realloc-based, but tracks total bytes and refuses any
  // growth past the per-plugin ceiling. A refused growth returns null, which Luau
  // turns into a catchable out-of-memory error — so a runaway allocation fails the
  // offending call instead of OOM-killing the whole process. `ud` is the owning host.
  static void* allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize);

  // Shared with the main-loop stream lambdas: the HttpClient stream id once known,
  // and a cancelled flag so a stop that races stream startup still cancels.
  struct HttpStreamControl {
    std::atomic<std::uint64_t> clientStreamId{0};
    std::atomic<bool> cancelled{false};
  };
  struct HttpStreamRecord {
    int lineRef = 0;
    int closeRef = 0;
    std::shared_ptr<HttpStreamControl> control;
  };

  void stopAllStreams() noexcept;
  void stopAllHttpStreams() noexcept;
  bool callGlobalInternal(const char* name, int args, std::chrono::milliseconds budget);
  bool callWithBudget(const char* name, int args, int results, std::chrono::milliseconds budget);
  void beginBudget(std::string_view name, std::chrono::milliseconds budget);
  void endBudget();

  std::uint64_t m_hostId = 0;
  scripting::ScriptApiContext& m_api;
  CompositorPlatform* m_platform = nullptr;
  scripting::PluginBindingContext* m_scriptContext = nullptr;
  std::filesystem::path m_pluginDir;
  std::string m_pluginId;
  scripting::PluginTranslationCatalog m_translations;
  std::unordered_set<int> m_stateWatchCallbackRefs;
  StateWatchHandler m_stateWatchHandler;
  std::unordered_set<int> m_streamCallbackRefs;
  // Active runStream children; `alive` clears on process exit so the slot can be reused.
  struct StreamRecord {
    std::shared_ptr<std::atomic<bool>> cancel;
    std::shared_ptr<std::atomic<bool>> alive;
  };
  std::vector<StreamRecord> m_streams;
  StreamLineHandler m_streamLineHandler;
  std::unordered_map<int, HttpStreamRecord> m_httpStreams; // keyed by stream key (line ref)
  HttpStreamEventHandler m_httpStreamEventHandler;
  lua_State* m_L = nullptr; // main state, frozen by luaL_sandbox
  lua_State* m_T = nullptr; // sandboxed thread; user code runs here
  int m_threadRef = -1;     // registry ref pinning m_T against the GC
  std::unordered_set<int> m_asyncCommandCallbackRefs;
  std::unordered_set<int> m_asyncProcessMatchCallbackRefs;
  std::unordered_set<int> m_asyncHttpCallbackRefs;
  std::unordered_set<int> m_colorPickerCallbackRefs;
  HttpClient* m_httpClient = nullptr;
  AsyncCommandResultHandler m_asyncCommandResultHandler;
  AsyncProcessMatchResultHandler m_asyncProcessMatchResultHandler;
  AsyncHttpResultHandler m_asyncHttpResultHandler;
  ColorPickerResultHandler m_colorPickerResultHandler;
  std::size_t m_memUsed = 0; // bytes tracked by allocate(); guarded by the worker-thread serialization
  std::chrono::nanoseconds m_callCpuDeadline{};
  std::string m_currentCallName;
  bool m_budgetActive = false;
  bool m_lastCallTimedOut = false;
  bool m_muteErrors = false;
};
