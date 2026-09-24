#pragma once

#include <chrono>
#include <functional>

// Fires a tick at the finest boundary any consumer currently needs. Consumers do
// not subscribe individually; the owning application publishes a Precision, so
// the poll source can park until the next displayed value can have changed
// instead of waking on a fixed cadence.
class TimeService {
public:
  using Clock = std::chrono::system_clock;
  using TimePoint = Clock::time_point;
  using SecondPoint = std::chrono::time_point<Clock, std::chrono::seconds>;
  using TickCallback = std::function<void()>;

  enum class Precision {
    // Wakes on each minute boundary. Enough for clocks that render HH:MM.
    Minute,
    // Wakes on each second boundary. Only armed while something actually
    // displays seconds.
    Second,
  };
  using PrecisionProvider = std::function<Precision()>;

  TimeService();

  void setTickCallback(TickCallback callback);
  // Queried during every poll preparation, so a boundary is armed as soon as a
  // component starts or stops needing time ticks. Defaults to Minute.
  void setPrecisionProvider(PrecisionProvider provider);

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();

  [[nodiscard]] TimePoint now() const noexcept { return m_now; }

private:
  [[nodiscard]] Precision precision() const;

  TickCallback m_tickCallback;
  PrecisionProvider m_precisionProvider;
  TimePoint m_now;
  SecondPoint m_nowSeconds;
};
