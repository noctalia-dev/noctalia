#pragma once

#include <condition_variable>
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

    void enqueue(std::string command);
    void waitIdle();
    size_t pendingCount() const;

  private:
    void workerLoop();

    std::deque<std::string> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::thread> m_workers;
    bool m_shutdown = false;
    size_t m_pending = 0;
    std::condition_variable m_idleCv;
  };

} // namespace noctalia::theme
