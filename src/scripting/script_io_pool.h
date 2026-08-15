#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace scripting {

  // Dedicated to potentially blocking plugin filesystem reads so they cannot
  // occupy ScriptWorkerPool threads that drain plugin VM events.
  class ScriptIoPool {
  public:
    static ScriptIoPool& instance();

    ScriptIoPool(const ScriptIoPool&) = delete;
    ScriptIoPool& operator=(const ScriptIoPool&) = delete;

    [[nodiscard]] bool post(std::function<void()> task);

  private:
    explicit ScriptIoPool(std::size_t workerCount = 2);
    ~ScriptIoPool();

    void runWorker();

    static constexpr std::size_t kMaxQueuedTasks = 64;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_tasks;
    std::vector<std::thread> m_workers;
    bool m_stopping = false;
  };

} // namespace scripting
