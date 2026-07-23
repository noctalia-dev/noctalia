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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace scripting {
  namespace {
    constexpr Logger kLog("script-runtime");
    constexpr std::size_t kMaxQueuedEvents = 64;
    std::atomic<int> g_shutdownSignal{0};

    // Unique per-State id, used to tag and clean up this runtime's state-store watchers.
    std::uint64_t nextStateToken() {
      static std::atomic<std::uint64_t> counter{1};
      return counter.fetch_add(1, std::memory_order_relaxed);
    }

    // Per-call budgets, spent in worker-thread CPU time (see threadCpuTime() in
    // luau_host.cpp).
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
      if (src.fontBaseline.has_value()) {
        dest.fontBaseline = src.fontBaseline;
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
        const std::vector<ScriptSideEffect>& effects, ClipboardService* clipboard, ScriptApiContext& api,
        const ScriptRuntime::TogglePanelCallback& togglePanelCallback
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
          if (togglePanelCallback) {
            togglePanelCallback(effect.title);
          } else {
            api.invokeTogglePanel(effect.title);
          }
          break;
        }
      }
    }

  } // namespace

  struct ScriptRuntime::State : public std::enable_shared_from_this<ScriptRuntime::State> {
    explicit State(
        std::string name, ScriptSettings widgetSettings, ScriptApiContext& api, std::filesystem::path dir,
        HttpClient* httpClientPtr, ClipboardService* clipboardService, TogglePanelCallback panelToggleCallback
    )
        : runtimeName(std::move(name)), settings(std::move(widgetSettings)), scriptApi(api), pluginDir(std::move(dir)),
          httpClient(httpClientPtr), clipboard(clipboardService), togglePanelCallback(std::move(panelToggleCallback)) {}

    mutable std::mutex mutex;
    std::condition_variable stopCv;
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
    TogglePanelCallback togglePanelCallback;
    SubscriberId nextSubscriberId = 1;
    std::uint64_t generation = 0;
    std::chrono::milliseconds updateInterval{250};
    std::chrono::steady_clock::time_point lastUpdateAccepted;
    std::vector<std::chrono::steady_clock::time_point> timeoutHistory;
    std::vector<std::chrono::steady_clock::time_point> errorHistory;
    ScriptResult replayState;
    bool replayStateReady = false;
    bool scheduled = false;
    bool stopping = false;
    bool stopped = false;
    bool updateQueued = false;
    bool updateRunning = false;
    bool hasOnIpc = false;
    bool hasOnIpcKnown = false;
    bool hasOnActivate = false;
    bool hasOnActivateKnown = false;
    bool hasOnConfigChanged = false;
    bool hasOnConfigChangedKnown = false;
    bool hasOnScroll = false;
    bool hasOnScrollKnown = false;
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

    void stop(int exitSignal) {
      bool shouldSchedule = false;
      {
        std::scoped_lock lock(mutex);
        if (stopped) {
          return;
        }
        if (!stopping) {
          stopping = true;
          queue.clear();
          updateQueued = false;
          subscribers.clear();

          ScriptEvent event;
          event.kind = ScriptEventKind::Stop;
          event.generation = generation;
          event.exitSignal = exitSignal;
          queue.push_back(std::move(event));
          if (!scheduled) {
            scheduled = true;
            shouldSchedule = true;
          }
        }
      }

      if (shouldSchedule) {
        auto self = shared_from_this();
        ScriptWorkerPool::instance().post([self] { self->drain(); });
      }

      std::unique_lock lock(mutex);
      stopCv.wait(lock, [this] { return stopped; });
    }

    bool enqueue(ScriptEvent event) {
      bool shouldSchedule = false;
      {
        std::scoped_lock lock(mutex);
        if (stopped || stopping) {
          return false;
        }
        if (unhealthy
            && event.kind != ScriptEventKind::Reload
            && event.kind != ScriptEventKind::Load
            && event.kind != ScriptEventKind::Stop) {
          return false;
        }

        // Supersede an already-queued coalescing CallArgs for the same callback
        // with the newer payload instead of appending. Bounds the queue to a
        // single pending event per callback (e.g. onAudioSpectrum at 60Hz), so a
        // slow script can never accumulate stale spectrum frames.
        if (event.kind == ScriptEventKind::CallArgs && event.coalesce) {
          const auto existing = std::ranges::find_if(queue, [&event](const auto& queued) {
            return queued.kind == ScriptEventKind::CallArgs
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
          if (event.droppable) {
            return false;
          }
          const auto droppable = std::ranges::find_if(queue, [](const auto& queued) {
            return queued.kind == ScriptEventKind::Update || queued.droppable;
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

    void enqueueColorPickerResult(std::uint64_t hostId, int callbackRef, std::optional<std::string> color) {
      ScriptEvent event;
      event.kind = ScriptEventKind::ColorPickerResult;
      event.hostId = hostId;
      event.callbackRef = callbackRef;
      event.colorPickerResult = std::move(color);
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

    void
    enqueueHttpStreamEvent(std::uint64_t hostId, int streamKey, bool closed, std::string line, bool ok, int status) {
      ScriptEvent event;
      event.kind = closed ? ScriptEventKind::HttpStreamClosed : ScriptEventKind::HttpStreamLine;
      event.hostId = hostId;
      event.callbackRef = streamKey;
      event.first = std::move(line);
      event.httpOk = ok;
      event.httpStatus = status;
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

        if (event.kind == ScriptEventKind::Stop) {
          teardownHost(event.exitSignal, event.snapshot);
          {
            std::scoped_lock lock(mutex);
            queue.clear();
            stopped = true;
            scheduled = false;
          }
          stopCv.notify_all();
          return;
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

      if (event.kind == ScriptEventKind::ColorPickerResult) {
        if (event.hostId != host->hostId() || !host->hasColorPickerCallback(event.callbackRef)) {
          return std::nullopt;
        }
        bindingContext.beginCall(event.snapshot);
        const bool ok = host->callColorPickerCallback(event.callbackRef, event.colorPickerResult, event.budget);
        return collectResult(event, "color picker callback", ok);
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

      if (event.kind == ScriptEventKind::HttpStreamLine) {
        if (event.hostId != host->hostId() || !host->hasHttpStream(event.callbackRef)) {
          return std::nullopt; // stale (reloaded host) or stopped stream
        }
        bindingContext.beginCall(event.snapshot);
        const bool ok = host->callHttpStreamLineCallback(event.callbackRef, event.first, event.budget);
        return collectResult(event, "http stream callback", ok);
      }

      if (event.kind == ScriptEventKind::HttpStreamClosed) {
        if (event.hostId != host->hostId() || !host->hasHttpStream(event.callbackRef)) {
          return std::nullopt;
        }
        bindingContext.beginCall(event.snapshot);
        const bool ok =
            host->callHttpStreamCloseCallback(event.callbackRef, event.httpOk, event.httpStatus, event.budget);
        return collectResult(event, "http stream close callback", ok);
      }

      if (event.kind == ScriptEventKind::SettingsChanged) {
        // Swap the live snapshot first, so getConfig() returns the new values
        // both inside onConfigChanged and on the next update().
        settings = event.newSettings;
        if (!host->hasGlobal("onConfigChanged")) {
          return std::nullopt;
        }
        bindingContext.beginCall(event.snapshot);
        const bool ok = host->callGlobalWithBudget("onConfigChanged", event.budget);
        return collectResult(event, "onConfigChanged", ok);
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
      case ScriptEventKind::CallArgs:
        if (!host->hasGlobal(event.functionName.c_str())) {
          return std::nullopt;
        }
        ok = host->callGlobalWithArgsAndBudget(event.functionName.c_str(), event.args, event.budget);
        break;
      default:
        break;
      }
      return collectResult(event, event.functionName, ok);
    }

    ScriptResult processLoad(const ScriptEvent& event) {
      teardownHost(0, event.snapshot);

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
      host->setColorPickerResultHandler(
          [weak](std::uint64_t hostId, int callbackRef, std::optional<std::string> color) {
            if (auto state = weak.lock()) {
              state->enqueueColorPickerResult(hostId, callbackRef, std::move(color));
            }
          }
      );
      host->setStreamLineHandler([weak](std::uint64_t hostId, int callbackRef, std::string line) {
        if (auto state = weak.lock()) {
          state->enqueueStreamLine(hostId, callbackRef, std::move(line));
        }
      });
      host->setHttpStreamEventHandler(
          [weak](std::uint64_t hostId, int streamKey, bool closed, std::string line, bool ok, int status) {
            if (auto state = weak.lock()) {
              state->enqueueHttpStreamEvent(hostId, streamKey, closed, std::move(line), ok, status);
            }
          }
      );

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
      const bool onConfigChangedPresent = host != nullptr && host->hasGlobal("onConfigChanged");
      const bool onScrollPresent = host != nullptr && host->hasGlobal("onScroll");
      {
        std::scoped_lock lock(mutex);
        hasOnIpc = result.hasOnIpc;
        hasOnIpcKnown = true;
        hasOnActivate = onActivatePresent;
        hasOnActivateKnown = true;
        hasOnConfigChanged = onConfigChangedPresent;
        hasOnConfigChangedKnown = true;
        hasOnScroll = onScrollPresent;
        hasOnScrollKnown = true;
      }
      return result;
    }

    void teardownHost(int signal, const ScriptSnapshot& snapshot) {
      // Prevent old or newly registered watchers from outliving this VM.
      PluginStateStore::instance().removeWatchers(stateToken);
      if (host != nullptr && host->hasGlobal("onExit")) {
        bindingContext.beginCall(snapshot);
        const ScriptArgs args{static_cast<double>(signal)};
        (void)host->callGlobalWithArgsAndBudget("onExit", args, kCallbackBudget);
      }
      PluginStateStore::instance().removeWatchers(stateToken);
      host.reset();
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
        result.error = result.timedOut ? "script callback exceeded its CPU budget" : "script callback failed";
      }

      if (result.patch.updateIntervalMs.has_value()) {
        std::scoped_lock lock(mutex);
        updateInterval = std::chrono::milliseconds(*result.patch.updateIntervalMs);
      }

      updateHealth(result);
      return result;
    }

    // Health verdict for a finished call. Two independent budgets feed `unhealthy`:
    // repeated CPU-budget overruns (a script that won't yield) and repeated hard
    // errors (a script that keeps throwing, including hitting the VM memory ceiling). When
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

      dispatchSideEffects(result.sideEffects, clipboard, scriptApi, togglePanelCallback);
      for (const auto& effect : result.sideEffects) {
        if (effect.kind == ScriptSideEffectKind::CopyToClipboard) {
          result.copiedToClipboard = true;
          break;
        }
      }
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
      HttpClient* httpClient, ClipboardService* clipboard, TogglePanelCallback togglePanelCallback
  )
      : m_state(
            std::make_shared<State>(
                std::move(runtimeName), std::move(settings), api, std::move(pluginDir), httpClient, clipboard,
                std::move(togglePanelCallback)
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
      m_state->stop(g_shutdownSignal.load(std::memory_order_relaxed));
    }
  }

  void ScriptRuntime::setShutdownSignal(int signal) noexcept {
    g_shutdownSignal.store(signal, std::memory_order_relaxed);
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

  bool ScriptRuntime::enqueueCallArgs(
      std::string functionName, ScriptArgs args, ScriptSnapshot snapshot, ScriptCallOptions options
  ) {
    ScriptEvent event;
    event.kind = ScriptEventKind::CallArgs;
    event.functionName = std::move(functionName);
    event.args = std::move(args);
    event.snapshot = std::move(snapshot);
    event.budget = kCallbackBudget;
    event.coalesce = options.coalesce;
    event.droppable = options.droppable;
    return m_state != nullptr && m_state->enqueue(std::move(event));
  }

  bool ScriptRuntime::enqueueCallBool(std::string functionName, bool value, ScriptSnapshot snapshot) {
    // Hover-style state echoes: a queue-full drop only loses an edge the next
    // event supersedes anyway.
    return enqueueCallArgs(std::move(functionName), {value}, std::move(snapshot), {.droppable = true});
  }

  bool ScriptRuntime::enqueueCallStrings(
      std::string functionName, std::string first, std::string second, ScriptSnapshot snapshot, bool coalesce
  ) {
    return enqueueCallArgs(
        std::move(functionName), {std::move(first), std::move(second)}, std::move(snapshot), {.coalesce = coalesce}
    );
  }

  bool ScriptRuntime::enqueueAsyncCommandResult(std::uint64_t hostId, int callbackRef, process::RunResult result) {
    if (m_state == nullptr) {
      return false;
    }
    m_state->enqueueAsyncResult(hostId, callbackRef, std::move(result));
    return true;
  }

  bool ScriptRuntime::enqueueSettingsChanged(ScriptSettings newSettings, ScriptSnapshot snapshot) {
    ScriptEvent event;
    event.kind = ScriptEventKind::SettingsChanged;
    event.newSettings = std::move(newSettings);
    event.snapshot = std::move(snapshot);
    event.budget = kCallbackBudget;
    return m_state != nullptr && m_state->enqueue(std::move(event));
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

  bool ScriptRuntime::hasOnConfigChanged() const {
    if (m_state == nullptr) {
      return false;
    }
    std::scoped_lock lock(m_state->mutex);
    return m_state->hasOnConfigChangedKnown && m_state->hasOnConfigChanged;
  }

  bool ScriptRuntime::hasOnScroll() const {
    if (m_state == nullptr) {
      return false;
    }
    std::scoped_lock lock(m_state->mutex);
    return m_state->hasOnScrollKnown && m_state->hasOnScroll;
  }

  bool ScriptRuntime::unhealthy() const {
    if (m_state == nullptr) {
      return true;
    }
    std::scoped_lock lock(m_state->mutex);
    return m_state->unhealthy;
  }

} // namespace scripting
