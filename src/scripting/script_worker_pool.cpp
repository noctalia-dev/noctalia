#include "scripting/script_worker_pool.h"

#include <algorithm>

namespace scripting {

  ScriptWorkerPool& ScriptWorkerPool::instance() {
    static ScriptWorkerPool pool;
    return pool;
  }

  ScriptWorkerPool::ScriptWorkerPool(std::size_t workerCount) {
    const auto count = std::clamp<std::size_t>(workerCount, 1, 8);
    m_workers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      m_workers.emplace_back([this] { runWorker(); });
    }
  }

  ScriptWorkerPool::~ScriptWorkerPool() {
    {
      std::scoped_lock lock(m_mutex);
      m_stopping = true;
    }
    m_cv.notify_all();
    for (auto& worker : m_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void ScriptWorkerPool::post(std::function<void()> task) {
    if (!task) {
      return;
    }
    {
      std::scoped_lock lock(m_mutex);
      if (m_stopping) {
        return;
      }
      m_tasks.push_back(std::move(task));
    }
    m_cv.notify_one();
  }

  void ScriptWorkerPool::runWorker() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
        if (m_stopping && m_tasks.empty()) {
          return;
        }
        task = std::move(m_tasks.front());
        m_tasks.pop_front();
      }
      if (task) {
        task();
      }
    }
  }

} // namespace scripting
