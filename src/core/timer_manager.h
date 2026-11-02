#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

class TimerManager {
public:
  using TimerId = std::uint64_t;

  static TimerManager& instance();

  TimerId
  start(TimerId existingId, std::chrono::milliseconds delay, std::function<void()> callback, bool repeating = false);
  bool cancel(TimerId id);
  bool active(TimerId id) const noexcept;
  int pollTimeoutMs() const;
  void tick();

private:
  TimerManager() = default;
};

class Timer {
public:
  Timer() = default;
  ~Timer();

  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

  Timer(Timer&& other) noexcept;
  Timer& operator=(Timer&& other) noexcept;

  void start(std::chrono::milliseconds delay, std::function<void()> callback);
  void startRepeating(std::chrono::milliseconds interval, std::function<void()> callback);
  void stop();
  bool active() const noexcept { return TimerManager::instance().active(m_id); }

private:
  TimerManager::TimerId m_id = 0;
};
