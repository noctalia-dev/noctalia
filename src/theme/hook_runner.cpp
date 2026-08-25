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
    }
    m_cv.notify_all();

    for (auto& worker : m_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void HookRunner::enqueue(std::string command) {
    {
      std::scoped_lock lock(m_mutex);
      if (m_shutdown) {
        return;
      }
      m_queue.push_back(std::move(command));
      ++m_pending;
    }
    m_cv.notify_one();
  }

  void HookRunner::waitIdle() {
    std::unique_lock lock(m_mutex);
    m_idleCv.wait(lock, [this]() { return m_shutdown || (m_pending == 0 && m_queue.empty()); });
  }

  size_t HookRunner::pendingCount() const {
    std::scoped_lock lock(m_mutex);
    return m_pending;
  }

  void HookRunner::workerLoop() {
    while (true) {
      std::string command;
      {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_shutdown || !m_queue.empty(); });

        if (m_shutdown && m_queue.empty()) {
          return;
        }

        if (m_queue.empty()) {
          continue;
        }

        command = std::move(m_queue.front());
        m_queue.pop_front();
      }

      const auto result = process::runSync(command);

      {
        std::scoped_lock lock(m_mutex);
        --m_pending;

        if (!result) {
          kLog.warn("hook failed with exit code {}: {} (command: {})", result.exitCode, result.err, command);
        }

        if (m_pending == 0) {
          m_idleCv.notify_all();
        }
      }
    }
  }

} // namespace noctalia::theme
