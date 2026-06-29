#include "app/main_loop.h"

#include "app/application.h"
#include "app/poll_source.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "shell/bar/bar.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cxxabi.h>
#include <format>
#include <limits>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <wayland-client-core.h>

namespace {
  constexpr Logger kLog("main");
  constexpr float kSlowMainLoopOperationDebugMs = 50.0f;
  constexpr float kSlowMainLoopOperationWarnMs = 1000.0f;
  constexpr auto kIdleProfileReportInterval = std::chrono::seconds(10);

  struct SourceIdleProfile {
    std::string name;
    std::uint64_t dispatchCalls = 0;
    std::uint64_t wakeDispatches = 0;
    std::uint64_t fdWakeups = 0;
    std::uint64_t timeoutWakeups = 0;
    std::uint64_t timeoutVotes = 0;
    std::uint64_t zeroTimeoutVotes = 0;
    double dispatchMs = 0.0;
    double maxDispatchMs = 0.0;
  };

  struct MainLoopIdleProfile {
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    std::uint64_t iterations = 0;
    std::uint64_t pollFdWakeups = 0;
    std::uint64_t pollTimeoutWakeups = 0;
    std::uint64_t pollImmediateWakeups = 0;
    std::uint64_t waylandFdWakeups = 0;
    std::uint64_t waylandReadableWakeups = 0;
    std::uint64_t waylandWritableWakeups = 0;
    std::uint64_t waylandErrorWakeups = 0;
    std::uint64_t deferredBatches = 0;
    std::uint64_t deferredCallbacks = 0;
    std::uint64_t waylandFlushCalls = 0;
    std::uint64_t waylandFlushBlocked = 0;
    std::uint64_t waylandReadCalls = 0;
    std::uint64_t waylandDispatchCalls = 0;
    std::uint64_t surfaceFrameWorkDrains = 0;
    std::uint64_t surfaceRenderDrains = 0;
    double deferredMs = 0.0;
    double pollPrepMs = 0.0;
    double pollWaitMs = 0.0;
    double waylandFlushMs = 0.0;
    double waylandReadMs = 0.0;
    double waylandDispatchMs = 0.0;
    double sourceDispatchMs = 0.0;
    double surfaceFrameWorkMs = 0.0;
    double surfaceRenderMs = 0.0;
    double processCpuStartMs = 0.0;
    double mainThreadCpuStartMs = 0.0;
    std::unordered_map<std::string, SourceIdleProfile> sources;
  };

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  bool idleProfileEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("NOCTALIA_IDLE_PROFILE");
      return value != nullptr
          && value[0] != '\0'
          && std::string_view(value) != "0"
          && std::string_view(value) != "false";
    }();
    return enabled;
  }

  double cpuClockMs(clockid_t clockId) {
    timespec ts{};
    if (::clock_gettime(clockId, &ts) != 0) {
      return 0.0;
    }
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1000000.0;
  }

  std::string demangleTypeName(const char* name) {
    int status = 0;
    std::unique_ptr<char, decltype(&std::free)> demangled{
        abi::__cxa_demangle(name, nullptr, nullptr, &status), &std::free
    };
    if (status == 0 && demangled != nullptr) {
      return demangled.get();
    }
    return name != nullptr ? std::string(name) : std::string("<unknown>");
  }

  MainLoopIdleProfile& idleProfile() {
    static MainLoopIdleProfile profile;
    return profile;
  }

  SourceIdleProfile& sourceIdleProfile(MainLoopIdleProfile& profile, const PollSource& source) {
    const std::string rawName = typeid(source).name();
    auto& entry = profile.sources[rawName];
    if (entry.name.empty()) {
      entry.name = demangleTypeName(rawName.c_str());
    }
    return entry;
  }

  void resetIdleProfile(MainLoopIdleProfile& profile, std::chrono::steady_clock::time_point now) {
    profile = MainLoopIdleProfile{};
    profile.windowStart = now;
    profile.processCpuStartMs = cpuClockMs(CLOCK_PROCESS_CPUTIME_ID);
    profile.mainThreadCpuStartMs = cpuClockMs(CLOCK_THREAD_CPUTIME_ID);
  }

  std::uint64_t requestTotal(const SurfaceIdleProfileEntry& entry) {
    return entry.requestUpdate + entry.requestUpdateOnly + entry.requestLayout + entry.requestRedraw;
  }

  void maybeReportIdleProfile() {
    if (!idleProfileEnabled()) {
      return;
    }

    auto& profile = idleProfile();
    const auto now = std::chrono::steady_clock::now();
    if (now - profile.windowStart < kIdleProfileReportInterval) {
      return;
    }

    const double seconds = std::chrono::duration<double>(now - profile.windowStart).count();
    const double processCpuMs = std::max(0.0, cpuClockMs(CLOCK_PROCESS_CPUTIME_ID) - profile.processCpuStartMs);
    const double mainThreadCpuMs = std::max(0.0, cpuClockMs(CLOCK_THREAD_CPUTIME_ID) - profile.mainThreadCpuStartMs);
    const double backgroundCpuMs = std::max(0.0, processCpuMs - mainThreadCpuMs);
    const double pctDenom = std::max(0.001, seconds * 10.0);
    const auto surfaceSnapshot = Surface::takeIdleProfileSnapshot(true);
    kLog.info(
        "idle profile {:.1f}s: loops={} cpu(process/thread/bg)={:.1f}/{:.1f}/{:.1f}ms "
        "{:.2f}/{:.2f}/{:.2f}% poll(fd/timeout/immediate)={}/{}/{} wlFd(total/in/out/err)={}/{}/{}/{} "
        "prep={:.2f}ms "
        "wait={:.1f}ms deferred={} callbacks {:.2f}ms wl(flush/read/dispatch)={}/{}/{} "
        "{:.2f}/{:.2f}/{:.2f}ms sourceDispatch={:.2f}ms surfaceDrains(frame/render)={}/{} {:.2f}/{:.2f}ms",
        seconds, profile.iterations, processCpuMs, mainThreadCpuMs, backgroundCpuMs, processCpuMs / pctDenom,
        mainThreadCpuMs / pctDenom, backgroundCpuMs / pctDenom, profile.pollFdWakeups, profile.pollTimeoutWakeups,
        profile.pollImmediateWakeups, profile.waylandFdWakeups, profile.waylandReadableWakeups,
        profile.waylandWritableWakeups, profile.waylandErrorWakeups, profile.pollPrepMs, profile.pollWaitMs,
        profile.deferredBatches, profile.deferredMs, profile.waylandFlushCalls, profile.waylandReadCalls,
        profile.waylandDispatchCalls, profile.waylandFlushMs, profile.waylandReadMs, profile.waylandDispatchMs,
        profile.sourceDispatchMs, profile.surfaceFrameWorkDrains, profile.surfaceRenderDrains,
        profile.surfaceFrameWorkMs, profile.surfaceRenderMs
    );

    std::vector<SourceIdleProfile> sources;
    sources.reserve(profile.sources.size());
    for (const auto& [name, entry] : profile.sources) {
      (void)name;
      sources.push_back(entry);
    }
    std::ranges::sort(sources, [](const auto& lhs, const auto& rhs) {
      if (lhs.wakeDispatches != rhs.wakeDispatches) {
        return lhs.wakeDispatches > rhs.wakeDispatches;
      }
      if (lhs.dispatchMs != rhs.dispatchMs) {
        return lhs.dispatchMs > rhs.dispatchMs;
      }
      return lhs.dispatchCalls > rhs.dispatchCalls;
    });

    std::size_t printed = 0;
    for (const auto& source : sources) {
      if (printed >= 10) {
        break;
      }
      if (source.dispatchCalls == 0 && source.timeoutVotes == 0) {
        continue;
      }
      ++printed;
      kLog.info(
          "idle profile source {}: wake={} fd={} timeout={} dispatch={} time={:.3f}ms max={:.3f}ms "
          "timeoutVotes={} zeroVotes={}",
          source.name, source.wakeDispatches, source.fdWakeups, source.timeoutWakeups, source.dispatchCalls,
          source.dispatchMs, source.maxDispatchMs, source.timeoutVotes, source.zeroTimeoutVotes
      );
    }

    const auto& total = surfaceSnapshot.total;
    kLog.info(
        "idle profile surfaces: requests(update/updateOnly/layout/redraw)={}/{}/{}/{} queued(frame/render)={}/{} "
        "processed(frame/render)={}/{} prepare={} {:.3f}ms frameTick={} {:.3f}ms animation={} {:.3f}ms "
        "updateCb={} {:.3f}ms renders={} {:.3f}ms",
        total.requestUpdate, total.requestUpdateOnly, total.requestLayout, total.requestRedraw, total.queuedFrameWork,
        total.queuedRenders, total.processedFrameWork, total.processedQueuedRenders, total.prepareCallbacks,
        total.prepareMs, total.frameTicks, total.frameTickMs, total.animationTicks, total.animationMs,
        total.updateCallbacks, total.updateMs, total.renders, total.renderMs
    );

    auto surfaces = surfaceSnapshot.surfaces;
    std::ranges::sort(surfaces, [](const auto& lhs, const auto& rhs) {
      if (lhs.renderMs != rhs.renderMs) {
        return lhs.renderMs > rhs.renderMs;
      }
      if (lhs.renders != rhs.renders) {
        return lhs.renders > rhs.renders;
      }
      return requestTotal(lhs) > requestTotal(rhs);
    });

    printed = 0;
    for (const auto& surface : surfaces) {
      if (printed >= 8) {
        break;
      }
      if (surface.renders == 0
          && surface.prepareCallbacks == 0
          && requestTotal(surface) == 0
          && surface.frameTicks == 0
          && surface.updateCallbacks == 0) {
        continue;
      }
      ++printed;
      kLog.info(
          "idle profile surface {}: requests={}/{}/{}/{} frame/render={}/{} prepare={} {:.3f}ms "
          "frameTick={} {:.3f}ms updateCb={} {:.3f}ms renders={} {:.3f}ms",
          surface.label, surface.requestUpdate, surface.requestUpdateOnly, surface.requestLayout, surface.requestRedraw,
          surface.processedFrameWork, surface.processedQueuedRenders, surface.prepareCallbacks, surface.prepareMs,
          surface.frameTicks, surface.frameTickMs, surface.updateCallbacks, surface.updateMs, surface.renders,
          surface.renderMs
      );
    }

    resetIdleProfile(profile, now);
  }

  template <typename... Args> void logSlowMainLoopOperation(float ms, std::format_string<Args...> fmt, Args&&... args) {
    if (ms >= kSlowMainLoopOperationWarnMs) {
      kLog.warn(fmt, std::forward<Args>(args)...);
    } else if (ms >= kSlowMainLoopOperationDebugMs) {
      kLog.debug(fmt, std::forward<Args>(args)...);
    }
  }

  [[noreturn]] void
  throwWaylandFailure(const WaylandConnection& wayland, std::string_view operation, int operationErrno) {
    throw std::runtime_error(std::format("{}: {}", operation, wayland.describeDisplayError(operationErrno)));
  }

  // libwayland-client dispatches every event listener through libffi (wl_closure_invoke). A C++
  // exception escaping a listener would unwind across that C ABI boundary and call std::terminate.
  // Contain it here so one misbehaving handler can't abort the shell; remaining queued events are
  // re-dispatched on the next iteration. Returns the wl_display_dispatch_pending result unchanged
  // (negative = genuine protocol error, still surfaced by the caller); a caught exception yields 0.
  int dispatchPendingGuarded(wl_display* display) {
    try {
      return wl_display_dispatch_pending(display);
    } catch (const std::exception& e) {
      kLog.error("exception in Wayland event handler ({}); continuing", e.what());
      return 0;
    } catch (...) {
      kLog.error("unknown exception in Wayland event handler; continuing");
      return 0;
    }
  }

  bool sourceHasReadyFd(const std::vector<pollfd>& fds, std::size_t startIdx, std::size_t count) {
    const std::size_t endIdx = std::min(fds.size(), startIdx + count);
    for (std::size_t fdIdx = startIdx; fdIdx < endIdx; ++fdIdx) {
      if (fds[fdIdx].revents != 0) {
        return true;
      }
    }
    return false;
  }
} // namespace

MainLoop::MainLoop(WaylandConnection& wayland, Bar& bar, PollSourcesProvider sourcesProvider)
    : m_wayland(wayland), m_bar(bar), m_sourcesProvider(std::move(sourcesProvider)) {}

void MainLoop::run() {
  if (idleProfileEnabled()) {
    kLog.info(
        "NOCTALIA_IDLE_PROFILE enabled; logging idle wake/render profile every {}s",
        std::chrono::duration_cast<std::chrono::seconds>(kIdleProfileReportInterval).count()
    );
    resetIdleProfile(idleProfile(), std::chrono::steady_clock::now());
  }

  while (!Application::s_shutdownRequested) {
    if (idleProfileEnabled()) {
      ++idleProfile().iterations;
    }

    // Process deferred callbacks from the previous iteration
    auto opStart = std::chrono::steady_clock::now();
    auto pending = DeferredCall::takePending();
    if (!pending.empty()) {
      const std::size_t count = pending.size();
      for (auto& fn : pending) {
        if (fn) {
          fn();
        }
      }
      const float ms = elapsedSince(opStart);
      if (idleProfileEnabled()) {
        auto& profile = idleProfile();
        ++profile.deferredBatches;
        profile.deferredCallbacks += count;
        profile.deferredMs += ms;
      }
      logSlowMainLoopOperation(ms, "deferred callbacks took {:.1f}ms (count={})", ms, count);
    }

    opStart = std::chrono::steady_clock::now();
    while (wl_display_prepare_read(m_wayland.display()) != 0) {
      opStart = std::chrono::steady_clock::now();
      if (dispatchPendingGuarded(m_wayland.display()) < 0) {
        const int dispatchErrno = errno;
        throwWaylandFailure(m_wayland, "failed to dispatch pending Wayland events before poll", dispatchErrno);
      }
      const float ms = elapsedSince(opStart);
      logSlowMainLoopOperation(ms, "wl_display_dispatch_pending took {:.1f}ms before poll", ms);
    }
    // Try to flush queued requests. If the kernel send buffer is full we get
    // EAGAIN; that is the standard Wayland backpressure signal, not a fatal
    // error. In that case ask poll() to also wake us when the fd is writable
    // and retry the flush before dispatching anything else.
    short waylandPollEvents = POLLIN;
    int flushRet = 0;
    opStart = std::chrono::steady_clock::now();
    do {
      flushRet = wl_display_flush(m_wayland.display());
    } while (flushRet < 0 && errno == EINTR);
    float ms = elapsedSince(opStart);
    if (idleProfileEnabled()) {
      auto& profile = idleProfile();
      ++profile.waylandFlushCalls;
      profile.waylandFlushMs += ms;
    }
    logSlowMainLoopOperation(ms, "wl_display_flush took {:.1f}ms before poll", ms);
    const bool flushBlocked = flushRet < 0;
    if (flushBlocked) {
      if (idleProfileEnabled()) {
        ++idleProfile().waylandFlushBlocked;
      }
      const int flushErrno = errno;
      if (flushErrno != EAGAIN) {
        wl_display_cancel_read(m_wayland.display());
        throwWaylandFailure(m_wayland, "failed to flush Wayland display before poll", flushErrno);
      }
      waylandPollEvents |= POLLOUT;
    }

    // Collect poll fds and compute timeout from all sources. The source list is
    // fetched fresh each iteration so config reloads can add/remove poll sources
    // (e.g. polkit/brightness) without leaving stale pointers in the loop.
    opStart = std::chrono::steady_clock::now();
    const std::vector<PollSource*> sources = m_sourcesProvider ? m_sourcesProvider() : std::vector<PollSource*>{};
    std::vector<pollfd> pollFds;
    pollFds.push_back({.fd = wl_display_get_fd(m_wayland.display()), .events = waylandPollEvents, .revents = 0});

    int pollTimeout = -1;
    std::vector<std::size_t> sourceStartIndices;
    std::vector<std::size_t> sourceFdCounts;
    sourceStartIndices.reserve(sources.size());
    sourceFdCounts.reserve(sources.size());

    // Forget deadlines for sources that no longer exist this iteration so the
    // map cannot accumulate stale entries or fire on a freed pointer.
    if (!m_sourceDeadlines.empty()) {
      const std::unordered_set<PollSource*> live(sources.begin(), sources.end());
      for (auto it = m_sourceDeadlines.begin(); it != m_sourceDeadlines.end();) {
        it = live.contains(it->first) ? std::next(it) : m_sourceDeadlines.erase(it);
      }
    }

    const auto nowBeforePoll = std::chrono::steady_clock::now();
    for (auto* source : sources) {
      const std::size_t startIdx = source->addPollFds(pollFds);
      sourceStartIndices.push_back(startIdx);
      sourceFdCounts.push_back(pollFds.size() - startIdx);

      const int t = source->pollTimeoutMs();
      if (idleProfileEnabled()) {
        auto& stats = sourceIdleProfile(idleProfile(), *source);
        if (t >= 0) {
          ++stats.timeoutVotes;
        }
        if (t == 0) {
          ++stats.zeroTimeoutVotes;
        }
      }

      if (t < 0) {
        // Source no longer wants a timed wake; drop any pending deadline.
        m_sourceDeadlines.erase(source);
        continue;
      }

      // Arm the deadline on first sight, then keep the earliest time the source
      // has requested since its last dispatch.
      const auto requested = nowBeforePoll + std::chrono::milliseconds(t);
      auto [it, inserted] = m_sourceDeadlines.try_emplace(source, requested);
      if (!inserted) {
        it->second = std::min(it->second, requested);
      }

      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(it->second - nowBeforePoll).count();
      const int remainingMs =
          remaining < 0 ? 0 : static_cast<int>(std::min<std::int64_t>(remaining, std::numeric_limits<int>::max()));
      if (pollTimeout < 0 || remainingMs < pollTimeout) {
        pollTimeout = remainingMs;
      }
    }
    if (Surface::hasPendingFrameWork() || Surface::hasPendingRenders()) {
      pollTimeout = 0;
    }

    // If the flush was blocked, raise the timeout floor so we actually wait
    // for POLLOUT instead of tight-looping with a 0-timeout source on top of
    // a full kernel buffer. ~16ms caps the spin at one frame at 60Hz.
    if (flushBlocked && pollTimeout >= 0 && pollTimeout < 16) {
      pollTimeout = 16;
    }

    ms = elapsedSince(opStart);
    if (idleProfileEnabled()) {
      idleProfile().pollPrepMs += ms;
    }
    logSlowMainLoopOperation(
        ms, "poll source preparation took {:.1f}ms (sources={} fds={} timeout={}ms)", ms, sources.size(),
        pollFds.size(), pollTimeout
    );

#ifndef NDEBUG
    // Spin canary: if a source votes pollTimeoutMs()==0 for >100ms continuously,
    // the loop is hot-looping. Asymmetric guards between pollTimeoutMs() and
    // dispatch() are the usual cause. Throttled to one warn per 5s.
    {
      using SteadyClock = std::chrono::steady_clock;
      static std::optional<SteadyClock::time_point> s_zeroSince;
      static std::optional<SteadyClock::time_point> s_lastWarn;
      const auto now = SteadyClock::now();
      if (pollTimeout == 0) {
        if (!s_zeroSince) {
          s_zeroSince = now;
        } else if (
            now - *s_zeroSince > std::chrono::milliseconds(100)
            && (!s_lastWarn || now - *s_lastWarn > std::chrono::seconds(5))
        ) {
          s_lastWarn = now;
          for (auto* src : sources) {
            if (src->pollTimeoutMs() == 0) {
              kLog.warn("main loop spin: {} keeps voting timeout=0", typeid(*src).name());
            }
          }
        }
      } else {
        s_zeroSince.reset();
      }
    }
#endif

    const auto pollStart = std::chrono::steady_clock::now();
    opStart = pollStart;
    const int pollResult = ::poll(pollFds.data(), pollFds.size(), pollTimeout);
    ms = elapsedSince(pollStart);
    if (idleProfileEnabled()) {
      auto& profile = idleProfile();
      profile.pollWaitMs += ms;
      if (pollResult == 0) {
        ++profile.pollTimeoutWakeups;
        if (pollTimeout == 0) {
          ++profile.pollImmediateWakeups;
        }
      } else if (pollResult > 0) {
        ++profile.pollFdWakeups;
        const short waylandRevents = pollFds[0].revents;
        if (waylandRevents != 0) {
          ++profile.waylandFdWakeups;
        }
        if ((waylandRevents & POLLIN) != 0) {
          ++profile.waylandReadableWakeups;
        }
        if ((waylandRevents & POLLOUT) != 0) {
          ++profile.waylandWritableWakeups;
        }
        if ((waylandRevents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          ++profile.waylandErrorWakeups;
        }
      }
    }
    if (pollResult < 0) {
      wl_display_cancel_read(m_wayland.display());
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("failed to poll fds");
    }

    // If we were waiting for the wayland fd to become writable, retry the
    // flush now. A persistent EAGAIN just defers to the next iteration.
    if ((waylandPollEvents & POLLOUT) != 0 && (pollFds[0].revents & POLLOUT) != 0) {
      opStart = std::chrono::steady_clock::now();
      do {
        flushRet = wl_display_flush(m_wayland.display());
      } while (flushRet < 0 && errno == EINTR);
      ms = elapsedSince(opStart);
      if (idleProfileEnabled()) {
        auto& profile = idleProfile();
        ++profile.waylandFlushCalls;
        profile.waylandFlushMs += ms;
      }
      logSlowMainLoopOperation(ms, "wl_display_flush took {:.1f}ms after POLLOUT", ms);
      const int flushErrno = errno;
      if (flushRet < 0 && flushErrno != EAGAIN) {
        wl_display_cancel_read(m_wayland.display());
        throwWaylandFailure(m_wayland, "failed to flush Wayland display after POLLOUT", flushErrno);
      }
    }

    // Read and dispatch Wayland events. Keep socket reads separate from
    // callback dispatch so stalls identify which half is actually slow.
    const bool waylandReadable = (pollFds[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0;
    if (waylandReadable) {
      opStart = std::chrono::steady_clock::now();
      if (wl_display_read_events(m_wayland.display()) < 0) {
        const int readErrno = errno;
        throwWaylandFailure(m_wayland, "failed to read Wayland events", readErrno);
      }
      ms = elapsedSince(opStart);
      if (idleProfileEnabled()) {
        auto& profile = idleProfile();
        ++profile.waylandReadCalls;
        profile.waylandReadMs += ms;
      }
      logSlowMainLoopOperation(ms, "wl_display_read_events took {:.1f}ms", ms);
    } else {
      wl_display_cancel_read(m_wayland.display());
    }

    opStart = std::chrono::steady_clock::now();
    if (dispatchPendingGuarded(m_wayland.display()) < 0) {
      const int dispatchErrno = errno;
      throwWaylandFailure(m_wayland, "failed to dispatch pending Wayland events after poll", dispatchErrno);
    }
    ms = elapsedSince(opStart);
    if (idleProfileEnabled()) {
      auto& profile = idleProfile();
      ++profile.waylandDispatchCalls;
      profile.waylandDispatchMs += ms;
    }
    if (waylandReadable) {
      logSlowMainLoopOperation(ms, "wl_display_dispatch_pending took {:.1f}ms after read", ms);
    } else {
      logSlowMainLoopOperation(ms, "wl_display_dispatch_pending took {:.1f}ms after poll", ms);
    }

    // Dispatch only sources that actually woke: an fd reported revents, or the
    // timeout the source advertised before poll has elapsed. A source callback
    // (notably config reload) can synchronously rebuild services and destroy
    // optional poll sources (e.g. polkit) mid-iteration. Re-check liveness
    // before each dispatch to avoid dereferencing a pointer that was valid when
    // we built `sources` but was freed by an earlier source in this same pass.
    const auto nowAfterPoll = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < sources.size(); ++i) {
      const bool fdReady = sourceHasReadyFd(pollFds, sourceStartIndices[i], sourceFdCounts[i]);
      const auto deadlineIt = m_sourceDeadlines.find(sources[i]);
      const bool timeoutWake = deadlineIt != m_sourceDeadlines.end() && nowAfterPoll >= deadlineIt->second;
      if (!fdReady && !timeoutWake) {
        continue;
      }

      auto* source = sources[i];
      const std::vector<PollSource*> latestSources =
          m_sourcesProvider ? m_sourcesProvider() : std::vector<PollSource*>{};
      if (!std::ranges::contains(latestSources, source)) {
        continue;
      }
      // Serviced now — restart its clock so the next timeout is measured fresh.
      m_sourceDeadlines.erase(source);
      opStart = std::chrono::steady_clock::now();
      source->dispatch(pollFds, sourceStartIndices[i]);
      ms = elapsedSince(opStart);
      if (idleProfileEnabled()) {
        auto& stats = sourceIdleProfile(idleProfile(), *source);
        ++stats.dispatchCalls;
        stats.dispatchMs += ms;
        stats.maxDispatchMs = std::max(stats.maxDispatchMs, static_cast<double>(ms));
        if (fdReady || timeoutWake) {
          ++stats.wakeDispatches;
        }
        if (fdReady) {
          ++stats.fdWakeups;
        }
        if (timeoutWake) {
          ++stats.timeoutWakeups;
        }
        idleProfile().sourceDispatchMs += ms;
      }
      logSlowMainLoopOperation(ms, "poll source {} dispatch took {:.1f}ms", typeid(*source).name(), ms);
    }

    const bool hadFrameWork = Surface::hasPendingFrameWork();
    opStart = std::chrono::steady_clock::now();
    Surface::drainPendingFrameWork();
    ms = elapsedSince(opStart);
    if (idleProfileEnabled() && hadFrameWork) {
      auto& profile = idleProfile();
      ++profile.surfaceFrameWorkDrains;
      profile.surfaceFrameWorkMs += ms;
    }
    logSlowMainLoopOperation(ms, "queued surface frame work took {:.1f}ms", ms);

    const bool hadRenders = Surface::hasPendingRenders();
    opStart = std::chrono::steady_clock::now();
    Surface::drainPendingRenders();
    ms = elapsedSince(opStart);
    if (idleProfileEnabled() && hadRenders) {
      auto& profile = idleProfile();
      ++profile.surfaceRenderDrains;
      profile.surfaceRenderMs += ms;
    }
    logSlowMainLoopOperation(ms, "queued surface rendering took {:.1f}ms", ms);

    maybeReportIdleProfile();
  }

  // Close all UI surfaces immediately and flush Wayland to make them disappear
  kLog.debug("closing bar surfaces for clean shutdown");
  m_bar.closeAllInstances();

  if (dispatchPendingGuarded(m_wayland.display()) < 0) {
    const int dispatchErrno = errno;
    kLog.warn(
        "failed to dispatch pending Wayland events during shutdown: {}", m_wayland.describeDisplayError(dispatchErrno)
    );
  }
  if (wl_display_flush(m_wayland.display()) < 0) {
    const int flushErrno = errno;
    kLog.warn("failed to flush Wayland display during shutdown: {}", m_wayland.describeDisplayError(flushErrno));
  }
}
