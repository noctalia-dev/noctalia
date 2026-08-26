#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace noctalia::theme {

  class HookRunner {
  public:
    explicit HookRunner(size_t threadCount = 0);
    ~HookRunner();

    HookRunner(const HookRunner&) = delete;
    HookRunner& operator=(const HookRunner&) = delete;

    // Queues a hook for async execution. Hooks whose generation predates the
    // current one are discarded so superseded requests cannot leave stale state.
    void enqueue(std::string command, std::uint64_t generation);
    void invalidateBefore(std::uint64_t generation);
    void waitIdle();
    size_t pendingCount() const;

  private:
    struct QueuedHook {
      std::string command;
      std::uint64_t generation = 0;
    };

    void workerLoop();

    std::deque<QueuedHook> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::thread> m_workers;
    bool m_shutdown = false;
    std::uint64_t m_currentGeneration = 0;
    size_t m_pending = 0;
    std::condition_variable m_idleCv;
  };

} // namespace noctalia::theme
