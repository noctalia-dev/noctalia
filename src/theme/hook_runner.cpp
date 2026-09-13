#include "theme/hook_runner.h"

#include "core/log.h"
#include "core/process/process.h"

#include <utility>

namespace noctalia::theme {

  namespace {
    constexpr Logger kLog("hook_runner");
    // Hook output is only read back for the failure warning.
    constexpr std::size_t kMaxHookOutputBytes = 8 * 1024;
  } // namespace

  HookRunner::HookRunner(
      std::size_t maxConcurrent, std::chrono::milliseconds shutdownGrace, std::shared_ptr<std::atomic<bool>> cancel
  )
      : m_state(std::make_shared<State>()), m_shutdownGrace(shutdownGrace) {
    m_state->maxConcurrent = maxConcurrent > 0 ? maxConcurrent : kDefaultMaxConcurrent;
    if (cancel) {
      m_state->cancel = std::move(cancel);
    }
  }

  HookRunner::~HookRunner() {
    requestShutdown();
    std::unique_lock lock(m_state->mutex);
    // An owner that already spent part of the grace hands its deadline down; on its own the
    // runner starts the budget here.
    const auto deadline = m_state->shutdownDeadline.value_or(std::chrono::steady_clock::now() + m_shutdownGrace);
    // Hooks that already started own the shared state; wait them out instead of
    // killing a command halfway through rewriting an application's config.
    const auto idle = [this]() { return m_state->running == 0; };
    if (m_state->idleCv.wait_until(lock, deadline, idle)) {
      return;
    }
    if (!m_state->cancel->exchange(true)) {
      kLog.warn(
          "{} hook(s) still running after {}s; terminating them", m_state->running,
          std::chrono::duration<double>(m_shutdownGrace).count()
      );
    }
    if (m_state->idleCv.wait_until(lock, deadline + kTerminateGrace, idle)) {
      return;
    }
    // Giving up is safe: the running hooks hold the state through their own callbacks.
    // A hook that called setsid escapes the process group and is orphaned rather than killed.
    kLog.warn("{} hook(s) did not stop; leaving them behind", m_state->running);
  }

  void HookRunner::setShutdownDeadline(std::chrono::steady_clock::time_point deadline) {
    std::scoped_lock lock(m_state->mutex);
    m_state->shutdownDeadline = deadline;
  }

  void HookRunner::requestShutdown() {
    std::scoped_lock lock(m_state->mutex);
    m_state->shutdown = true;
    m_state->queue.clear();
    // Releases waitIdle() callers: the queued backlog is gone and running hooks
    // are awaited by the destructor, not by whoever was draining.
    m_state->idleCv.notify_all();
  }

  void HookRunner::enqueue(std::string command, std::uint64_t generation) {
    if (command.empty()) {
      return;
    }
    {
      std::scoped_lock lock(m_state->mutex);
      if (m_state->shutdown || generation < m_state->currentGeneration) {
        return;
      }
      m_state->queue.emplace_back(QueuedHook{std::move(command), generation});
    }
    pump(m_state);
  }

  void HookRunner::invalidateBefore(std::uint64_t generation) {
    std::scoped_lock lock(m_state->mutex);
    if (generation <= m_state->currentGeneration) {
      return;
    }
    m_state->currentGeneration = generation;
    std::erase_if(m_state->queue, [generation](const QueuedHook& hook) { return hook.generation < generation; });
    if (m_state->queue.empty() && m_state->running == 0) {
      m_state->idleCv.notify_all();
    }
  }

  void HookRunner::waitIdle() {
    std::unique_lock lock(m_state->mutex);
    m_state->idleCv.wait(lock, [this]() {
      return m_state->shutdown || (m_state->queue.empty() && m_state->running == 0);
    });
  }

  std::size_t HookRunner::pendingCount() const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->queue.size() + m_state->running;
  }

  void HookRunner::pump(const std::shared_ptr<State>& state) {
    for (;;) {
      std::string command;
      {
        std::scoped_lock lock(state->mutex);
        while (!state->shutdown && state->running < state->maxConcurrent && !state->queue.empty()) {
          QueuedHook hook = std::move(state->queue.front());
          state->queue.pop_front();
          if (hook.generation < state->currentGeneration) {
            continue;
          }
          command = std::move(hook.command);
          ++state->running;
          break;
        }

        if (command.empty()) {
          if (state->queue.empty() && state->running == 0) {
            state->idleCv.notify_all();
          }
          return;
        }
      }

      if (!launch(state, command)) {
        std::scoped_lock lock(state->mutex);
        --state->running;
      }
    }
  }

  bool HookRunner::launch(const std::shared_ptr<State>& state, const std::string& command) {
    process::RunCallbacks callbacks;
    callbacks.onExit = [state, command](process::RunResult result) {
      if (!result) {
        kLog.warn("hook failed with exit code {}: {} (command: {})", result.exitCode, result.err, command);
      }
      {
        std::scoped_lock lock(state->mutex);
        --state->running;
      }
      pump(state);
    };

    process::RunOptions options;
    options.maxOutputBytes = kMaxHookOutputBytes;
    options.cancel = state->cancel;
    if (process::runAsync(command, std::move(callbacks), options)) {
      return true;
    }
    kLog.warn("failed to start hook (command: {})", command);
    return false;
  }

} // namespace noctalia::theme
