#include "time/time_service.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

  // Round up: a sub-millisecond remainder must still wake after the boundary,
  // never before it, or the tick would see the previous second and do nothing.
  template <typename Unit> [[nodiscard]] int millisUntilNextBoundary(std::chrono::system_clock::time_point now) {
    using namespace std::chrono;
    const auto next = floor<Unit>(now) + Unit{1};
    const auto remaining = ceil<milliseconds>(next - now).count();
    return static_cast<int>(std::max<std::int64_t>(1, remaining));
  }

  // Ticks only fire on a new whole second, so a deadline wakes at the second
  // boundary it falls in, and never earlier than the next one: a past deadline
  // must not spin the loop until the second rolls over.
  [[nodiscard]] int
  millisUntilDeadline(std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point deadline) {
    using namespace std::chrono;
    const auto target = std::max(ceil<seconds>(deadline), floor<seconds>(now) + seconds{1});
    const auto remaining = ceil<milliseconds>(target - now).count();
    return static_cast<int>(std::clamp<std::int64_t>(remaining, 1, std::numeric_limits<int>::max()));
  }

} // namespace

TimeService::TimeService() {
  using namespace std::chrono;
  m_now = system_clock::now();
  m_nowSeconds = floor<seconds>(m_now);
}

void TimeService::setTickCallback(TickCallback callback) { m_tickCallback = std::move(callback); }

void TimeService::setPrecisionProvider(PrecisionProvider provider) { m_precisionProvider = std::move(provider); }

void TimeService::setDeadlineProvider(DeadlineProvider provider) { m_deadlineProvider = std::move(provider); }

TimeService::Precision TimeService::precision() const {
  return m_precisionProvider ? m_precisionProvider() : Precision::Minute;
}

int TimeService::pollTimeoutMs() const {
  const auto now = Clock::now();
  const int boundary = precision() == Precision::Second ? millisUntilNextBoundary<std::chrono::seconds>(now)
                                                        : millisUntilNextBoundary<std::chrono::minutes>(now);
  if (m_deadlineProvider) {
    if (const auto deadline = m_deadlineProvider()) {
      return std::min(boundary, millisUntilDeadline(now, *deadline));
    }
  }
  return boundary;
}

void TimeService::tick() {
  using namespace std::chrono;
  m_now = system_clock::now();
  const auto floored = floor<seconds>(m_now);

  if (floored != m_nowSeconds) {
    m_nowSeconds = floored;
    if (m_tickCallback) {
      m_tickCallback();
    }
  }
}

void TimeService::forceTick() {
  m_nowSeconds = {};
  tick();
}
