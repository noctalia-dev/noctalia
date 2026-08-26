#include "theme/hook_runner.h"

#include "core/log.h"
#include "core/process/process.h"

#include <algorithm>

namespace noctalia::theme {

  namespace {
    constexpr Logger kLog("hook_runner");
  }

  HookRunner::HookRunner(size_t threadCount) {
    if (threadCount == 0) {
      const unsigned int hw = std::thread::hardware_concurrency();
      threadCount = std::max(2U, hw / 2);
    }

    threadCount = std::min(threadCount, size_t{8});

    kLog.debug("starting hook runner with {} threads", threadCount);

    m_workers.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
      m_workers.emplace_back([this]() { workerLoop(); });
    }
  }

  HookRunner::~HookRunner() {
    {
      std::scoped_lock lock(m_mutex);
      m_shutdown = true;
      // Drop queued-but-not-started hooks; only running hooks finish.
      m_pending -= m_queue.size();
      m_queue.clear();
    }
    m_cv.notify_all();

    for (auto& worker : m_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void HookRunner::enqueue(std::string command, std::uint64_t generation) {
    {
      std::scoped_lock lock(m_mutex);
      if (m_shutdown || generation < m_currentGeneration) {
        return;
      }
      m_queue.emplace_back(QueuedHook{std::move(command), generation});
      ++m_pending;
    }
    m_cv.notify_one();
  }

  void HookRunner::invalidateBefore(std::uint64_t generation) {
    std::scoped_lock lock(m_mutex);
    if (generation <= m_currentGeneration) {
      return;
    }
    m_currentGeneration = generation;

    for (auto it = m_queue.begin(); it != m_queue.end();) {
      if (it->generation < m_currentGeneration) {
        it = m_queue.erase(it);
        --m_pending;
      } else {
        ++it;
      }
    }
    if (m_pending == 0) {
      m_idleCv.notify_all();
    }
  }

  void HookRunner::waitIdle() {
    std::unique_lock lock(m_mutex);
    m_idleCv.wait(lock, [this]() { return m_shutdown || m_pending == 0; });
  }

  size_t HookRunner::pendingCount() const {
    std::scoped_lock lock(m_mutex);
    return m_pending;
  }

  void HookRunner::workerLoop() {
    while (true) {
      QueuedHook item;
      {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_shutdown || !m_queue.empty(); });

        if (m_shutdown) {
          return;
        }

        if (m_queue.empty()) {
          continue;
        }

        item = std::move(m_queue.front());
        m_queue.pop_front();
      }

      // Re-check staleness in case invalidateBefore() advanced the generation
      // between the pop above and releasing the lock.
      {
        std::scoped_lock lock(m_mutex);
        if (item.generation < m_currentGeneration) {
          --m_pending;
          if (m_pending == 0) {
            m_idleCv.notify_all();
          }
          continue;
        }
      }

      const auto result = process::runSync(item.command);

      {
        std::scoped_lock lock(m_mutex);
        --m_pending;

        if (!result && item.generation >= m_currentGeneration) {
          kLog.warn("hook failed with exit code {}: {} (command: {})", result.exitCode, result.err, item.command);
        }

        if (m_pending == 0) {
          m_idleCv.notify_all();
        }
      }
    }
  }

} // namespace noctalia::theme
