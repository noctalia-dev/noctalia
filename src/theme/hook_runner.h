#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace noctalia::theme {

  // Runs template hooks concurrently with bounded parallelism. The runner owns no
  // threads: each hook is spawned through process::runAsync and reports completion
  // on that call's own thread, which keeps the shared state alive past destruction.
  class HookRunner {
  public:
    static constexpr std::size_t kDefaultMaxConcurrent = 4;
    // How long shutdown waits for a running hook before cancelling it. Long enough for a
    // normal hook to finish writing an application's config.
    static constexpr auto kDefaultShutdownGrace = std::chrono::seconds(5);
    // Allowance added past the grace for SIGTERM/SIGKILL to land and the hook to be reaped.
    static constexpr auto kTerminateGrace = std::chrono::milliseconds(1000);

    // `cancel`, when given, is the owner's cancel flag: the runner raises and observes the
    // same flag so a hook cannot be cancelled twice over, once per teardown ladder.
    explicit HookRunner(
        std::size_t maxConcurrent = kDefaultMaxConcurrent,
        std::chrono::milliseconds shutdownGrace = kDefaultShutdownGrace,
        std::shared_ptr<std::atomic<bool>> cancel = nullptr
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
    // Adopts the deadline of an owner that is already spending its own shutdown grace, so
    // destruction waits out the time that is left instead of starting a grace period over.
    void setShutdownDeadline(std::chrono::steady_clock::time_point deadline);
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
      // Set by an owner that is already spending its shutdown grace; see setShutdownDeadline.
      std::optional<std::chrono::steady_clock::time_point> shutdownDeadline;
      // Raised when a running hook outlives the shutdown grace, so it cannot hold up the quit.
      // Shared with the owner when it passed one in, so both see a single cancellation.
      std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    };

    static void pump(const std::shared_ptr<State>& state);
    [[nodiscard]] static bool launch(const std::shared_ptr<State>& state, const std::string& command);

    std::shared_ptr<State> m_state;
    std::chrono::milliseconds m_shutdownGrace;
  };

} // namespace noctalia::theme
