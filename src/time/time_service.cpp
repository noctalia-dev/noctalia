#include "time/time_service.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

namespace {

  // Round up: a sub-millisecond remainder must still wake after the boundary,
  // never before it, or the tick would see the previous second and do nothing.
  template <typename Unit> [[nodiscard]] int millisUntilNextBoundary() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto next = floor<Unit>(now) + Unit{1};
    const auto remaining = ceil<milliseconds>(next - now).count();
    return static_cast<int>(std::max<std::int64_t>(1, remaining));
  }

} // namespace

TimeService::TimeService() {
  using namespace std::chrono;
  m_now = system_clock::now();
  m_nowSeconds = floor<seconds>(m_now);
}

void TimeService::setTickCallback(TickCallback callback) { m_tickCallback = std::move(callback); }

void TimeService::setPrecisionProvider(PrecisionProvider provider) { m_precisionProvider = std::move(provider); }

TimeService::Precision TimeService::precision() const {
  return m_precisionProvider ? m_precisionProvider() : Precision::Minute;
}

int TimeService::pollTimeoutMs() const {
  if (precision() == Precision::Second) {
    return millisUntilNextBoundary<std::chrono::seconds>();
  }
  return millisUntilNextBoundary<std::chrono::minutes>();
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
