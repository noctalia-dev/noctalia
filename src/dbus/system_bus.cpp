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

bool SystemBus::systemdUnitEnabled(std::string_view unit) const {
  auto proxy = sdbus::createProxy(
      *m_connection, sdbus::ServiceName{"org.freedesktop.systemd1"}, sdbus::ObjectPath{"/org/freedesktop/systemd1"}
  );
  try {
    std::string state;
    proxy->callMethod("GetUnitFileState")
        .onInterface("org.freedesktop.systemd1.Manager")
        .withArguments(std::string{unit})
        .storeResultsTo(state);
    return state == "enabled";
  } catch (const sdbus::Error& e) {
    kLog.debug("GetUnitFileState({}) failed: {}", unit, e.what());
    return false;
  }
}

// Directed signals reach the handler even when another peer sent them, so callers check the sender here.
bool SystemBus::isNameOwner(std::string_view name, std::string_view sender) const {
  try {
    auto proxy = sdbus::createProxy(*m_connection, kDbusName, kDbusPath);
    std::string owner;
    proxy->callMethod("GetNameOwner")
        .onInterface(kDbusInterface)
        .withArguments(std::string{name})
        .storeResultsTo(owner);
    return owner == sender;
  } catch (const sdbus::Error& e) {
    kLog.debug("GetNameOwner({}) failed: {}", name, e.what());
    return false;
  }
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
