#include "scripting/script_runtime.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "scripting/luau_host.h"
#include "scripting/plugin_bindings.h"
#include "scripting/plugin_state_store.h"
#include "scripting/script_api_context.h"
#include "scripting/script_worker_pool.h"
#include "scripting/ui_prelude.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace scripting {
  namespace {
    constexpr Logger kLog("script-runtime");
    constexpr std::size_t kMaxQueuedEvents = 64;

    // Unique per-State id, used to tag and clean up this runtime's state-store watchers.
    std::uint64_t nextStateToken() {
      static std::atomic<std::uint64_t> counter{1};
      return counter.fetch_add(1, std::memory_order_relaxed);
    }

    constexpr auto kLoadBudget = std::chrono::milliseconds(100);
    constexpr auto kUpdateBudget = std::chrono::milliseconds(12);
    constexpr auto kCallbackBudget = std::chrono::milliseconds(25);
    // Error/crash budget: too many timeouts or hard errors in a window marks the
    // runtime unhealthy, which stops feeding it events until the next reload.
    constexpr auto kTimeoutWindow = std::chrono::seconds(60);
    constexpr int kMaxConsecutiveTimeouts = 3;
    constexpr std::size_t kMaxTimeoutsPerWindow = 5;
    constexpr auto kCrashWindow = std::chrono::seconds(60);
    constexpr std::size_t kMaxHardErrorsPerWindow = 5;

    void mergePatch(ScriptPatch& dest, const ScriptPatch& src) {
      if (src.text.has_value()) {
        dest.text = src.text;
      }
      if (src.glyph.has_value()) {
        dest.glyph = src.glyph;
      }
      if (src.image.has_value()) {
        dest.image = src.image;
      }
      if (src.tooltip.has_value()) {
        dest.tooltip = src.tooltip;
      }
      if (src.fontFamily.has_value()) {
        dest.fontFamily = src.fontFamily;
      }
      if (src.textColor.has_value()) {
        dest.textColor = src.textColor;
      }
      if (src.glyphColor.has_value()) {
        dest.glyphColor = src.glyphColor;
      }
      if (src.visible.has_value()) {
        dest.visible = src.visible;
      }
      if (src.updateIntervalMs.has_value()) {
        dest.updateIntervalMs = src.updateIntervalMs;
      }
      if (src.label.has_value()) {
        dest.label = src.label;
      }
      if (src.iconOn.has_value()) {
        dest.iconOn = src.iconOn;
      }
      if (src.iconOff.has_value()) {
        dest.iconOff = src.iconOff;
      }
      if (src.active.has_value()) {
        dest.active = src.active;
      }
      if (src.enabled.has_value()) {
        dest.enabled = src.enabled;
      }
      if (src.launcherResults.has_value()) {
        dest.launcherResults = src.launcherResults;
      }
      if (src.uiTree.has_value()) {
        dest.uiTree = src.uiTree;
      }
      if (src.wantsSecondTicks.has_value()) {
        dest.wantsSecondTicks = src.wantsSecondTicks;
      }
      if (src.needsFrameTick.has_value()) {
        dest.needsFrameTick = src.needsFrameTick;
      }
    }

    void mergeResult(ScriptResult& dest, const ScriptResult& src) {
      mergePatch(dest.patch, src.patch);
      dest.sideEffects.insert(dest.sideEffects.end(), src.sideEffects.begin(), src.sideEffects.end());
      dest.ok = dest.ok && src.ok;
      dest.timedOut = dest.timedOut || src.timedOut;
      if (!src.error.empty()) {
        dest.error = src.error;
      }
      if (src.hasOnIpcKnown) {
        dest.hasOnIpc = src.hasOnIpc;
        dest.hasOnIpcKnown = true;
      }
      dest.unhealthy = dest.unhealthy || src.unhealthy;
    }

    void dispatchSideEffects(
        const std::vector<ScriptSideEffect>& effects, ClipboardService* clipboard, ScriptApiContext& api
    ) {
      for (const auto& effect : effects) {
        switch (effect.kind) {
        case ScriptSideEffectKind::Log:
          kLog.info("{}", effect.title);
          break;
        case ScriptSideEffectKind::NotifyInfo:
          notify::info("Noctalia", effect.title, effect.body);
          break;
        case ScriptSideEffectKind::NotifyError:
          notify::error("Noctalia", effect.title, effect.body);
          break;
        case ScriptSideEffectKind::CopyToClipboard:
          if (clipboard == nullptr || !clipboard->copyText(effect.title, effect.body)) {
            kLog.warn("scripted clipboard copy failed");
          }
          break;
        case ScriptSideEffectKind::SetWallpaperEnabled:
          api.invokeWallpaperEnabled(effect.title, effect.flag);
          break;
        case ScriptSideEffectKind::SetWallpaper:
          api.invokeSetWallpaper(effect.title, effect.body);
          break;
        case ScriptSideEffectKind::TogglePanel:
          api.invokeTogglePanel(effect.title);
          break;
        }
      }
    }

  } // namespace

  struct ScriptRuntime::State : public std::enable_shared_from_this<ScriptRuntime::State> {
    explicit State(
        std::string name, ScriptSettings widgetSettings, ScriptApiContext& api, std::filesystem::path dir,
        HttpClient* httpClientPtr, ClipboardService* clipboardService
    )
        : runtimeName(std::move(name)), settings(std::move(widgetSettings)), scriptApi(api), pluginDir(std::move(dir)),
          httpClient(httpClientPtr), clipboard(clipboardService) {}

    mutable std::mutex mutex;
    std::string runtimeName;
    ScriptSettings settings;
    ScriptApiContext& scriptApi;
    std::filesystem::path pluginDir;
    HttpClient* httpClient = nullptr;
    const std::uint64_t stateToken = nextStateToken();
    std::deque<ScriptEvent> queue;
    std::unordered_map<SubscriberId, ScriptResultCallback> subscribers;
    std::unique_ptr<LuauHost> host;
    PluginBindingContext bindingContext;
    ClipboardService* clipboard = nullptr;
    SubscriberId nextSubscriberId = 1;
    std::uint64_t generation = 0;
    std::chrono::milliseconds updateInterval{250};
    std::chrono::steady_clock::time_point lastUpdateAccepted;
    std::vector<std::chrono::steady_clock::time_point> timeoutHistory;
    std::vector<std::chrono::steady_clock::time_point> errorHistory;
    ScriptResult replayState;
    bool replayStateReady = false;
    bool scheduled = false;
    bool stopped = false;
    bool updateQueued = false;
    bool updateRunning = false;
    bool hasOnIpc = false;
    bool hasOnIpcKnown = false;
    bool hasOnActivate = false;
    bool hasOnActivateKnown = false;
    bool unhealthy = false;
    int consecutiveTimeouts = 0;

    SubscriberId subscribe(ScriptResultCallback callback) {
      if (!callback) {
        return 0;
      }
      SubscriberId id = 0;
      ScriptResult replay;
      bool hasReplay = false;
      {
        std::scoped_lock lock(mutex);
        id = nextSubscriberId++;
        subscribers[id] = std::move(callback);
        if (replayStateReady) {
          replay = replayState;
          hasReplay = true;
        }
      }

      if (!hasReplay) {
        return id;
      }

      auto self = shared_from_this();
      DeferredCall::callLater([self, id, replay = std::move(replay)]() mutable {
        ScriptResultCallback subscriber;
        {
          std::scoped_lock replayLock(self->mutex);
          if (self->stopped || replay.generation != self->generation) {
            return;
          }
          auto it = self->subscribers.find(id);
          if (it == self->subscribers.end()) {
            return;
          }
          subscriber = it->second;
        }

        if (subscriber) {
          subscriber(std::move(replay));
        }
      });

      return id;
    }

    void unsubscribe(SubscriberId id) {
      std::scoped_lock lock(mutex);
      subscribers.erase(id);
    }

    void stop() {
      PluginStateStore::instance().removeWatchers(stateToken);
      std::scoped_lock lock(mutex);
      stopped = true;
      queue.clear();
      subscribers.clear();
    }

    bool enqueue(ScriptEvent event) {
      bool shouldSchedule = false;
      {
        std::scoped_lock lock(mutex);
        if (stopped) {
          return false;
        }
        if (unhealthy
            && event.kind != ScriptEventKind::Reload
            && event.kind != ScriptEventKind::Load
            && event.kind != ScriptEventKind::Stop) {
          return false;
        }

        // Supersede an already-queued coalescing CallStrings for the same
        // callback with the newer payload instead of appending. Bounds the queue
        // to a single pending event per callback (e.g. onAudioSpectrum at 60Hz),
        // so a slow script can never accumulate stale spectrum frames.
        if (event.kind == ScriptEventKind::CallStrings && event.coalesce) {
          const auto existing = std::ranges::find_if(queue, [&event](const auto& queued) {
            return queued.kind == ScriptEventKind::CallStrings
                && queued.coalesce
                && queued.functionName == event.functionName;
          });
          if (existing != queue.end()) {
            event.generation = generation;
            *existing = std::move(event);
            return true;
          }
        }

        if (event.kind == ScriptEventKind::Update) {
          const auto now = std::chrono::steady_clock::now();
          if (updateQueued || updateRunning) {
            return true;
          }
          if (lastUpdateAccepted.time_since_epoch().count() != 0
              && now - lastUpdateAccepted
                  < std::max(updateInterval - std::chrono::milliseconds(5), std::chrono::milliseconds(1))) {
            return true;
          }
          updateQueued = true;
          lastUpdateAccepted = now;
        }

        if (queue.size() >= kMaxQueuedEvents) {
          if (event.kind == ScriptEventKind::Update) {
            updateQueued = false;
            return false;
          }
          if (event.kind == ScriptEventKind::CallBool) {
            return false;
          }
          const auto droppable = std::ranges::find_if(queue, [](const auto& queued) {
            return queued.kind == ScriptEventKind::Update || queued.kind == ScriptEventKind::CallBool;
          });
          if (droppable != queue.end()) {
            if (droppable->kind == ScriptEventKind::Update) {
              updateQueued = false;
            }
            queue.erase(droppable);
          } else {
            return false;
          }
        }

        if (event.kind == ScriptEventKind::Load || event.kind == ScriptEventKind::Reload) {
          ++generation;
          event.generation = generation;
          queue.clear();
          updateQueued = false;
          updateRunning = false;
          lastUpdateAccepted = {};
          replayState = {};
          replayStateReady = false;
          unhealthy = false;
          consecutiveTimeouts = 0;
          timeoutHistory.clear();
          errorHistory.clear();
        } else {
          event.generation = generation;
        }

        queue.push_back(std::move(event));
        if (!scheduled) {
          scheduled = true;
          shouldSchedule = true;
        }
      }

      if (shouldSchedule) {
        auto self = shared_from_this();
        ScriptWorkerPool::instance().post([self] { self->drain(); });
      }
      return true;
    }

    void enqueueAsyncResult(std::uint64_t hostId, int callbackRef, process::RunResult result) {
      ScriptEvent event;
      event.kind = ScriptEventKind::AsyncCommandResult;
      event.hostId = hostId;
      event.callbackRef = callbackRef;
      event.commandResult = std::move(result);
      event.budget = kCallbackBudget;
      (void)enqueue(std::move(event));
    }

    void enqueueAsyncProcessMatchResult(std::uint64_t hostId, int callbackRef, bool matched) {
      ScriptEvent event;
      event.kind = ScriptEventKind::AsyncProcessMatchResult;
      event.hostId = hostId;
      event.callbackRef = callbackRef;
      event.processMatchResult = matched;
      event.budget = kCallbackBudget;
      (void)enqueue(std::move(event));
    }

    void enqueueAsyncHttpResult(
        std::uint64_t hostId, int callbackRef, bool ok, int status, std::string body, bool isDownload
    ) {
      ScriptEvent event;
      event.kind = ScriptEventKind::AsyncHttpResult;
      event.hostId = hostId;
      event.callbackRef = callbackRef;
      event.httpOk = ok;
      event.httpStatus = status;
      event.httpBody = std::move(body);
      event.httpIsDownload = isDownload;
      event.budget = kCallbackBudget;
      (void)enqueue(std::move(event));
    }

    void enqueueStateWatchResult(int callbackRef, std::string json) {
      ScriptEvent event;
      event.kind = ScriptEventKind::StateWatchResult;
      event.callbackRef = callbackRef;
      event.stateJson = std::move(json);
      event.budget = kCallbackBudget;
      (void)enqueue(std::move(event));
    }

    void enqueueStreamLine(std::uint64_t hostId, int callbackRef, std::string line) {
      ScriptEvent event;
      event.kind = ScriptEventKind::StreamLine;
      event.hostId = hostId;
      event.callbackRef = callbackRef;
      event.first = std::move(line);
      event.budget = kCallbackBudget;
      (void)enqueue(std::move(event));
    }

    void drain() {
      for (;;) {
        ScriptEvent event;
        {
          std::scoped_lock lock(mutex);
          if (queue.empty() || stopped) {
            scheduled = false;
            return;
          }
          event = std::move(queue.front());
          queue.pop_front();
          if (event.kind == ScriptEventKind::Update) {
            updateQueued = false;
            updateRunning = true;
          }
        }

        auto result = processEvent(event);

        {
          std::scoped_lock lock(mutex);
          if (event.kind == ScriptEventKind::Update) {
            updateRunning = false;
          }
        }

        if (result.has_value()) {
          postResult(std::move(*result));
        }
      }
    }

    std::optional<ScriptResult> processEvent(const ScriptEvent& event) {
      if (event.kind == ScriptEventKind::Stop) {
        return std::nullopt;
      }

      if (event.kind == ScriptEventKind::Load || event.kind == ScriptEventKind::Reload) {
        return processLoad(event);
      }

      if (host == nullptr) {
        return std::nullopt;
      }

      if (event.kind == ScriptEventKind::AsyncCommandResult) {
        if (event.hostId != host->hostId() || !host->hasAsyncCommandCallback(event.callbackRef)) {
          return std::nullopt;
        }
        bindingContext.beginCall(event.snapshot);
        const bool ok = host->callAsyncCommandCallback(event.callbackRef, event.commandResult, event.budget);
        return collectResult(event, "async command callback", ok);
      }

      if (event.kind == ScriptEventKind::AsyncProcessMatchResult) {
        if (event.hostId != host->hostId() || !host->hasAsyncProcessMatchCallback(event.callbackRef)) {
          return std::nullopt;
        }
        bindingContext.beginCall(event.snapshot);
        const bool ok = host->callAsyncProcessMatchCallback(event.callbackRef, event.processMatchResult, event.budget);
        return collectResult(event, "process match callback", ok);
      }

      if (event.kind == ScriptEventKind::AsyncHttpResult) {
        if (event.hostId != host->hostId() || !host->hasAsyncHttpCallback(event.callbackRef)) {
          return std::nullopt;
        }
        bindingContext.beginCall(event.snapshot);
        const bool ok = event.httpIsDownload
            ? host->callAsyncDownloadCallback(event.callbackRef, event.httpOk, event.budget)
            : host->callAsyncHttpCallback(
                  event.callbackRef, event.httpOk, event.httpStatus, event.httpBody, event.budget
              );
        return collectResult(event, "http callback", ok);
      }

      if (event.kind == ScriptEventKind::StateWatchResult) {
        if (!host->hasStateWatchCallback(event.callbackRef)) {
          return std::nullopt;
        }
        bindingContext.beginCall(event.snapshot);
        const bool ok = host->callStateWatchCallback(event.callbackRef, event.stateJson, event.budget);
        return collectResult(event, "state watch callback", ok);
      }

      if (event.kind == ScriptEventKind::StreamLine) {
        if (event.hostId != host->hostId() || !host->hasStreamCallback(event.callbackRef)) {
          return std::nullopt; // stale (reloaded host) or unregistered
        }
        bindingContext.beginCall(event.snapshot);
        const bool ok = host->callStreamCallback(event.callbackRef, event.first, event.budget);
        return collectResult(event, "stream callback", ok);
      }

      bindingContext.beginCall(event.snapshot);
      bool ok = false;
      switch (event.kind) {
      case ScriptEventKind::Update:
      case ScriptEventKind::Call:
        if (!host->hasGlobal(event.functionName.c_str())) {
          return std::nullopt;
        }
        ok = host->callGlobalWithBudget(event.functionName.c_str(), event.budget);
        break;
      case ScriptEventKind::CallBool:
        if (!host->hasGlobal(event.functionName.c_str())) {
          return std::nullopt;
        }
        ok = host->callGlobalWithBoolAndBudget(event.functionName.c_str(), event.boolValue, event.budget);
        break;
      case ScriptEventKind::CallStrings:
        if (!host->hasGlobal(event.functionName.c_str())) {
          return std::nullopt;
        }
        ok = host->callGlobalWithStringsAndBudget(event.functionName.c_str(), event.first, event.second, event.budget);
        break;
      default:
        break;
      }
      return collectResult(event, event.functionName, ok);
    }

    ScriptResult processLoad(const ScriptEvent& event) {
      // Drop watchers registered by the previous load before re-registering below.
      PluginStateStore::instance().removeWatchers(stateToken);

      host = std::make_unique<LuauHost>(scriptApi);
      bindingContext.settings = &settings;
      bindingContext.host = host.get();
      bindingContext.ownerId = runtimeName;
      host->setPluginDir(pluginDir);
      host->setPluginId(runtimeName.substr(0, runtimeName.find(':')));
      host->loadTranslations();
      host->setHttpClient(httpClient);
      host->setScriptContext(&bindingContext);
      registerPluginBindings(host->state(), &bindingContext);

      std::weak_ptr<State> weak = shared_from_this();
      const std::string pluginId = runtimeName.substr(0, runtimeName.find(':'));
      const std::uint64_t token = stateToken;
      host->setStateWatchHandler([weak, pluginId, token](std::string key, int callbackRef) {
        PluginStateStore::instance().watch(pluginId, key, token, [weak, callbackRef](const std::string& json) {
          if (auto state = weak.lock()) {
            state->enqueueStateWatchResult(callbackRef, json);
          }
        });
      });
      host->setAsyncCommandResultHandler([weak](std::uint64_t hostId, int callbackRef, process::RunResult result) {
        if (auto state = weak.lock()) {
          state->enqueueAsyncResult(hostId, callbackRef, std::move(result));
        }
      });
      host->setAsyncProcessMatchResultHandler([weak](std::uint64_t hostId, int callbackRef, bool matched) {
        if (auto state = weak.lock()) {
          state->enqueueAsyncProcessMatchResult(hostId, callbackRef, matched);
        }
      });
      host->setAsyncHttpResultHandler(
          [weak](std::uint64_t hostId, int callbackRef, bool ok, int status, std::string body, bool isDownload) {
            if (auto state = weak.lock()) {
              state->enqueueAsyncHttpResult(hostId, callbackRef, ok, status, std::move(body), isDownload);
            }
          }
      );
      host->setStreamLineHandler([weak](std::uint64_t hostId, int callbackRef, std::string line) {
        if (auto state = weak.lock()) {
          state->enqueueStreamLine(hostId, callbackRef, std::move(line));
        }
      });

      ScriptResult result;
      result.generation = event.generation;
      result.callbackName = "load";

      bindingContext.beginCall(event.snapshot);
      if (!host->exec("=ui-prelude", kUiPrelude)) {
        kLog.warn("plugin {}: failed to install ui prelude", runtimeName);
      }
      bool ok = host->loadString(event.chunkName, event.source) && host->run();
      mergeResult(result, collectResult(event, "load", ok));

      if (ok) {
        ScriptEvent updateEvent = event;
        updateEvent.kind = ScriptEventKind::Update;
        updateEvent.functionName = "update";
        updateEvent.budget = kUpdateBudget;
        if (host->hasGlobal("update")) {
          bindingContext.beginCall(event.snapshot);
          ok = host->callGlobalWithBudget("update", kUpdateBudget);
          mergeResult(result, collectResult(updateEvent, "update", ok));
        }
      }

      result.hasOnIpc = host != nullptr && host->hasGlobal("onIpc");
      result.hasOnIpcKnown = true;
      const bool onActivatePresent = host != nullptr && host->hasGlobal("onActivate");
      {
        std::scoped_lock lock(mutex);
        hasOnIpc = result.hasOnIpc;
        hasOnIpcKnown = true;
        hasOnActivate = onActivatePresent;
        hasOnActivateKnown = true;
      }
      return result;
    }

    ScriptResult collectResult(const ScriptEvent& event, std::string_view callbackName, bool ok) {
      ScriptResult result;
      result.generation = event.generation;
      result.callbackName = std::string(callbackName);
      result.ok = ok;
      result.timedOut = host != nullptr && host->lastCallTimedOut();
      result.patch = bindingContext.patch;
      result.sideEffects = bindingContext.sideEffects;
      result.hasOnIpcKnown = false;
      if (!ok) {
        result.error = result.timedOut ? "script execution timed out" : "script callback failed";
      }

      if (result.patch.updateIntervalMs.has_value()) {
        std::scoped_lock lock(mutex);
        updateInterval = std::chrono::milliseconds(*result.patch.updateIntervalMs);
      }

      updateHealth(result);
      return result;
    }

    // Health verdict for a finished call. Two independent budgets feed `unhealthy`:
    // repeated timeouts (a script that won't return) and repeated hard errors (a
    // script that keeps throwing — including hitting the VM memory ceiling). When
    // either trips, the runtime is auto-disabled (enqueue() drops further events
    // until reload) and the user is notified once.
    void updateHealth(ScriptResult& result) {
      const char* reason = nullptr;
      {
        std::scoped_lock lock(mutex);
        const bool wasUnhealthy = unhealthy;
        const auto now = std::chrono::steady_clock::now();

        if (result.timedOut) {
          ++consecutiveTimeouts;
          timeoutHistory.push_back(now);
          std::erase_if(timeoutHistory, [now](const auto& ts) { return now - ts > kTimeoutWindow; });
          if (consecutiveTimeouts >= kMaxConsecutiveTimeouts || timeoutHistory.size() >= kMaxTimeoutsPerWindow) {
            unhealthy = true;
          }
        } else {
          consecutiveTimeouts = 0;
          if (!result.ok) {
            errorHistory.push_back(now);
            std::erase_if(errorHistory, [now](const auto& ts) { return now - ts > kCrashWindow; });
            if (errorHistory.size() >= kMaxHardErrorsPerWindow) {
              unhealthy = true;
            }
          }
        }

        result.unhealthy = unhealthy;
        if (unhealthy && !wasUnhealthy) {
          reason = result.timedOut ? "timeouts" : "errors";
        }
      }

      if (reason != nullptr) {
        notifyUnhealthy(reason);
      }
    }

    // Surface the auto-disable to the user. Runs on the main thread (notify is not
    // worker-thread safe); fired once per unhealthy episode.
    void notifyUnhealthy(std::string reason) {
      const std::string name = runtimeName;
      DeferredCall::callLater([name, reason = std::move(reason)] {
        const std::string bodyKey =
            reason == "timeouts" ? "plugins.auto-disabled.body-timeouts" : "plugins.auto-disabled.body-errors";
        notify::error("Noctalia", i18n::tr("plugins.auto-disabled.title"), i18n::tr(bodyKey, "plugin", name));
      });
    }

    void postResult(ScriptResult result) {
      auto self = shared_from_this();
      DeferredCall::callLater([self, result = std::move(result)]() mutable { self->deliverResult(std::move(result)); });
    }

    void deliverResult(ScriptResult result) {
      std::vector<ScriptResultCallback> callbacks;
      {
        std::scoped_lock lock(mutex);
        if (result.generation != generation || stopped) {
          return;
        }

        if (replayState.generation != generation) {
          replayState = {};
          replayState.generation = generation;
        }

        mergePatch(replayState.patch, result.patch);
        replayState.hasOnIpcKnown = result.hasOnIpcKnown || replayState.hasOnIpcKnown;
        if (result.hasOnIpcKnown) {
          replayState.hasOnIpc = result.hasOnIpc;
        }
        replayState.unhealthy = result.unhealthy;
        replayState.ok = replayState.ok && result.ok;
        replayState.timedOut = replayState.timedOut || result.timedOut;
        replayState.error = result.error;
        replayState.callbackName = result.callbackName;
        replayState.sideEffects.clear();
        replayStateReady = !replayState.patch.empty() || replayState.hasOnIpcKnown;

        callbacks.reserve(subscribers.size());
        for (const auto& [id, callback] : subscribers) {
          (void)id;
          callbacks.push_back(callback);
        }
      }

      dispatchSideEffects(result.sideEffects, clipboard, scriptApi);
      result.sideEffects.clear();

      for (auto& callback : callbacks) {
        if (callback) {
          callback(result);
        }
      }
    }
  };

  ScriptRuntime::ScriptRuntime(
      std::string runtimeName, ScriptSettings settings, ScriptApiContext& api, std::filesystem::path pluginDir,
      HttpClient* httpClient, ClipboardService* clipboard
  )
      : m_state(
            std::make_shared<State>(
                std::move(runtimeName), std::move(settings), api, std::move(pluginDir), httpClient, clipboard
            )
        ) {}

  ScriptRuntime::~ScriptRuntime() { stop(); }

  ScriptRuntime::SubscriberId ScriptRuntime::subscribe(ScriptResultCallback callback) {
    return m_state != nullptr ? m_state->subscribe(std::move(callback)) : 0;
  }

  void ScriptRuntime::unsubscribe(SubscriberId id) {
    if (m_state != nullptr) {
      m_state->unsubscribe(id);
    }
  }

  void ScriptRuntime::stop() {
    if (m_state != nullptr) {
      m_state->stop();
    }
  }

  void ScriptRuntime::start(std::string chunkName, std::string source, ScriptSnapshot snapshot) {
    reload(std::move(chunkName), std::move(source), std::move(snapshot));
  }

  void ScriptRuntime::reload(std::string chunkName, std::string source, ScriptSnapshot snapshot) {
    if (m_state == nullptr) {
      return;
    }
    ScriptEvent event;
    event.kind = ScriptEventKind::Reload;
    event.chunkName = std::move(chunkName);
    event.source = std::move(source);
    event.snapshot = std::move(snapshot);
    event.budget = kLoadBudget;
    (void)m_state->enqueue(std::move(event));
  }

  bool ScriptRuntime::enqueueUpdate(ScriptSnapshot snapshot) {
    ScriptEvent event;
    event.kind = ScriptEventKind::Update;
    event.functionName = "update";
    event.snapshot = std::move(snapshot);
    event.budget = kUpdateBudget;
    return m_state != nullptr && m_state->enqueue(std::move(event));
  }

  bool ScriptRuntime::enqueueCall(std::string functionName, ScriptSnapshot snapshot) {
    ScriptEvent event;
    event.kind = ScriptEventKind::Call;
    event.functionName = std::move(functionName);
    event.snapshot = std::move(snapshot);
    event.budget = kCallbackBudget;
    return m_state != nullptr && m_state->enqueue(std::move(event));
  }

  bool ScriptRuntime::enqueueCallBool(std::string functionName, bool value, ScriptSnapshot snapshot) {
    ScriptEvent event;
    event.kind = ScriptEventKind::CallBool;
    event.functionName = std::move(functionName);
    event.boolValue = value;
    event.snapshot = std::move(snapshot);
    event.budget = kCallbackBudget;
    return m_state != nullptr && m_state->enqueue(std::move(event));
  }

  bool ScriptRuntime::enqueueCallStrings(
      std::string functionName, std::string first, std::string second, ScriptSnapshot snapshot, bool coalesce
  ) {
    ScriptEvent event;
    event.kind = ScriptEventKind::CallStrings;
    event.functionName = std::move(functionName);
    event.first = std::move(first);
    event.second = std::move(second);
    event.snapshot = std::move(snapshot);
    event.budget = kCallbackBudget;
    event.coalesce = coalesce;
    return m_state != nullptr && m_state->enqueue(std::move(event));
  }

  bool ScriptRuntime::enqueueAsyncCommandResult(std::uint64_t hostId, int callbackRef, process::RunResult result) {
    if (m_state == nullptr) {
      return false;
    }
    m_state->enqueueAsyncResult(hostId, callbackRef, std::move(result));
    return true;
  }

  bool ScriptRuntime::hasOnIpc() const {
    if (m_state == nullptr) {
      return false;
    }
    std::scoped_lock lock(m_state->mutex);
    return m_state->hasOnIpcKnown && m_state->hasOnIpc;
  }

  bool ScriptRuntime::hasOnActivate() const {
    if (m_state == nullptr) {
      return false;
    }
    std::scoped_lock lock(m_state->mutex);
    return m_state->hasOnActivateKnown && m_state->hasOnActivate;
  }

  bool ScriptRuntime::unhealthy() const {
    if (m_state == nullptr) {
      return true;
    }
    std::scoped_lock lock(m_state->mutex);
    return m_state->unhealthy;
  }

} // namespace scripting
