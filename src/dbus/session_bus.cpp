#include "dbus/session_bus.h"

#include "core/log.h"

#include <chrono>

namespace {
  constexpr Logger kLog("session_bus");
  constexpr int kMaxEventsPerDispatch = 16;
  constexpr auto kMaxDispatchBudget = std::chrono::milliseconds(4);
  constexpr double kSlowEventDebugMs = 50.0;
  constexpr double kSlowEventWarnMs = 1000.0;

  double elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  void logSlowEvent(double ms) {
    if (ms >= kSlowEventWarnMs) {
      kLog.warn("single event dispatch took {:.1F}ms", ms);
    } else if (ms >= kSlowEventDebugMs) {
      kLog.debug("single event dispatch took {:.1F}ms", ms);
    }
  }
} // namespace

SessionBus::SessionBus() : m_connection(sdbus::createSessionBusConnection()) {}

sdbus::IConnection::PollData SessionBus::getPollData() const { return m_connection->getEventLoopPollData(); }

void SessionBus::processPendingEvents() {
  m_hasPendingEvents = false;

  const auto batchStart = std::chrono::steady_clock::now();
  for (int processed = 0; processed < kMaxEventsPerDispatch; ++processed) {
    const auto eventStart = std::chrono::steady_clock::now();
    const bool processedEvent = m_connection->processPendingEvent();
    if (!processedEvent) {
      return;
    }

    logSlowEvent(elapsedMs(eventStart));

    if (processed + 1 >= kMaxEventsPerDispatch || std::chrono::steady_clock::now() - batchStart >= kMaxDispatchBudget) {
      m_hasPendingEvents = true;
      return;
    }
  }
}
