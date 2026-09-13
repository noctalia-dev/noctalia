#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace noctalia::theme {

  // Runs template hooks concurrently with bounded parallelism. The runner owns no
  // threads: each hook is spawned through process::runAsync and reports completion
  // on that call's own thread, which keeps the shared state alive past destruction.
  class HookRunner {
  public:
    static constexpr std::size_t kDefaultMaxConcurrent = 4;
    // How long destruction waits for a running hook before cancelling it, and again before
    // giving up on it. Long enough for a normal hook to finish writing an application's config.
    static constexpr auto kDefaultShutdownGrace = std::chrono::seconds(5);

    explicit HookRunner(
        std::size_t maxConcurrent = kDefaultMaxConcurrent,
        std::chrono::milliseconds shutdownGrace = kDefaultShutdownGrace
    );
    ~HookRunner();

    HookRunner(const HookRunner&) = delete;
    HookRunner& operator=(const HookRunner&) = delete;

    // Starts a hook, or queues it while maxConcurrent hooks are already running.
    // Hooks whose generation predates the current one are discarded so superseded
    // requests cannot leave stale state.
    void enqueue(std::string command, std::uint64_t generation);
    // Discards queued hooks from older generations. Hooks that already started
    // keep running; waitIdle() waits them out.
    void invalidateBefore(std::uint64_t generation);
    void waitIdle();
    // Drops the queued backlog and releases waitIdle() callers. Running hooks keep
    // going; the destructor waits for them.
    void requestShutdown();
    [[nodiscard]] std::size_t pendingCount() const;

  private:
    struct QueuedHook {
      std::string command;
      std::uint64_t generation = 0;
    };

    struct State {
      std::mutex mutex;
      std::condition_variable idleCv;
      std::deque<QueuedHook> queue;
      std::size_t running = 0;
      std::size_t maxConcurrent = kDefaultMaxConcurrent;
      std::uint64_t currentGeneration = 0;
      bool shutdown = false;
      // Raised when a running hook outlives the shutdown grace, so it cannot hold up the quit.
      std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    };

    static void pump(const std::shared_ptr<State>& state);
    [[nodiscard]] static bool launch(const std::shared_ptr<State>& state, const std::string& command);

    std::shared_ptr<State> m_state;
    std::chrono::milliseconds m_shutdownGrace;
  };

} // namespace noctalia::theme
