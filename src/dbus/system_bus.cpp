#include "dbus/system_bus.h"

#include "core/log.h"

#include <chrono>
#include <string>

namespace {
  constexpr Logger kLog("system_bus");
  constexpr auto kDbusInterface = "org.freedesktop.DBus";
  const sdbus::ServiceName kDbusName{kDbusInterface};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};

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

SystemBus::SystemBus() : m_connection(sdbus::createSystemBusConnection()) {}

sdbus::IConnection::PollData SystemBus::getPollData() const { return m_connection->getEventLoopPollData(); }

bool SystemBus::nameHasOwner(std::string_view name) const {
  auto proxy = sdbus::createProxy(*m_connection, kDbusName, kDbusPath);
  bool hasOwner = false;
  proxy->callMethod("NameHasOwner")
      .onInterface(kDbusInterface)
      .withArguments(std::string{name})
      .storeResultsTo(hasOwner);
  return hasOwner;
}

void SystemBus::processPendingEvents() {
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
