#include "core/timer_manager.h"

#include "core/log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cxxabi.h>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

  constexpr Logger kLog("timer");
  constexpr float kSlowTimerCallbackDebugMs = 16.0f;
  constexpr float kSlowTimerCallbackWarnMs = 1000.0f;

  struct TimerEntry {
    TimerManager::TimerId id = 0;
    std::chrono::steady_clock::time_point dueAt{};
    std::chrono::milliseconds interval{0};
    std::function<void()> callback;
    bool repeating = false;
  };

  std::unordered_map<TimerManager::TimerId, TimerEntry>& timerEntries() {
    static std::unordered_map<TimerManager::TimerId, TimerEntry> entries;
    return entries;
  }

  TimerManager::TimerId& nextTimerId() {
    static TimerManager::TimerId nextId = 1;
    return nextId;
  }

  std::unordered_set<TimerManager::TimerId>& canceledTimerIds() {
    static std::unordered_set<TimerManager::TimerId> ids;
    return ids;
  }

  std::unordered_set<TimerManager::TimerId>& inFlightTimerIds() {
    static std::unordered_set<TimerManager::TimerId> ids;
    return ids;
  }

  std::string demangleTypeName(const char* name) {
    int status = 0;
    std::unique_ptr<char, decltype(&std::free)> demangled{
        abi::__cxa_demangle(name, nullptr, nullptr, &status), &std::free
    };
    if (status == 0 && demangled != nullptr) {
      return demangled.get();
    }
    return name != nullptr ? std::string(name) : std::string("<unknown>");
  }

} // namespace

TimerManager& TimerManager::instance() {
  static TimerManager manager;
  return manager;
}

TimerManager::TimerId TimerManager::start(
    TimerId existingId, std::chrono::milliseconds delay, std::function<void()> callback, bool repeating
) {
  if (existingId != 0) {
    cancel(existingId);
  }

  if (!callback) {
    return 0;
  }

  const TimerId id = nextTimerId()++;
  timerEntries()[id] = TimerEntry{
      .id = id,
      .dueAt = std::chrono::steady_clock::now() + std::max(delay, std::chrono::milliseconds(0)),
      .interval = std::max(delay, std::chrono::milliseconds(0)),
      .callback = std::move(callback),
      .repeating = repeating,
  };
  return id;
}

bool TimerManager::cancel(TimerId id) {
  if (id == 0) {
    return false;
  }

  if (timerEntries().erase(id) > 0) {
    return true;
  }

  if (inFlightTimerIds().contains(id)) {
    canceledTimerIds().insert(id);
    return true;
  }

  return false;
}

bool TimerManager::active(TimerId id) const noexcept {
  if (id == 0 || canceledTimerIds().contains(id)) {
    return false;
  }
  return timerEntries().contains(id) || inFlightTimerIds().contains(id);
}

int TimerManager::pollTimeoutMs() const {
  if (timerEntries().empty()) {
    return -1;
  }

  const auto now = std::chrono::steady_clock::now();
  auto nextDue = std::chrono::steady_clock::time_point::max();
  for (const auto& [id, entry] : timerEntries()) {
    (void)id;
    nextDue = std::min(nextDue, entry.dueAt);
  }

  if (nextDue <= now) {
    return 0;
  }

  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(nextDue - now).count();
  return static_cast<int>(std::min<std::int64_t>(remaining, std::numeric_limits<int>::max()));
}

void TimerManager::tick() {
  if (timerEntries().empty()) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  std::vector<TimerEntry> dueEntries;
  dueEntries.reserve(timerEntries().size());

  for (auto it = timerEntries().begin(); it != timerEntries().end();) {
    if (it->second.dueAt > now) {
      ++it;
      continue;
    }

    inFlightTimerIds().insert(it->second.id);
    dueEntries.push_back(std::move(it->second));
    it = timerEntries().erase(it);
  }

  for (auto& entry : dueEntries) {
    if (canceledTimerIds().contains(entry.id)) {
      canceledTimerIds().erase(entry.id);
      inFlightTimerIds().erase(entry.id);
      continue;
    }

    if (entry.callback) {
      const auto callbackStart = std::chrono::steady_clock::now();
      entry.callback();
      const float callbackMs =
          std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - callbackStart).count();
      if (callbackMs >= kSlowTimerCallbackWarnMs) {
        const auto callbackType = demangleTypeName(entry.callback.target_type().name());
        kLog.warn("timer callback {} id={} took {:.1f}ms", callbackType, entry.id, callbackMs);
      } else if (callbackMs >= kSlowTimerCallbackDebugMs) {
        const auto callbackType = demangleTypeName(entry.callback.target_type().name());
        kLog.debug("timer callback {} id={} took {:.1f}ms", callbackType, entry.id, callbackMs);
      }
    }

    if (entry.repeating && entry.id != 0 && !canceledTimerIds().contains(entry.id)) {
      auto next = entry;
      next.dueAt = std::chrono::steady_clock::now() + next.interval;
      timerEntries()[entry.id] = std::move(next);
    }

    canceledTimerIds().erase(entry.id);
    inFlightTimerIds().erase(entry.id);
  }
}

Timer::~Timer() { stop(); }

Timer::Timer(Timer&& other) noexcept : m_id(other.m_id) { other.m_id = 0; }

Timer& Timer::operator=(Timer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  stop();
  m_id = other.m_id;
  other.m_id = 0;
  return *this;
}

void Timer::start(std::chrono::milliseconds delay, std::function<void()> callback) {
  m_id = TimerManager::instance().start(m_id, delay, std::move(callback), false);
}

void Timer::startRepeating(std::chrono::milliseconds interval, std::function<void()> callback) {
  m_id = TimerManager::instance().start(m_id, interval, std::move(callback), true);
}

void Timer::stop() {
  if (m_id == 0) {
    return;
  }
  TimerManager::instance().cancel(m_id);
  m_id = 0;
}
