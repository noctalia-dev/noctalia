#include "dbus/logind/logind_service.h"

#include "dbus/system_bus.h"

#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <utility>

namespace {
  const sdbus::ServiceName kLogindBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLogindObjectPath{"/org/freedesktop/login1"};
  constexpr auto kLogindManagerInterface = "org.freedesktop.login1.Manager";
} // namespace

LogindService::LogindService(SystemBus& bus) : m_bus(bus) {
  m_managerProxy = sdbus::createProxy(m_bus.connection(), kLogindBusName, kLogindObjectPath);
  m_managerProxy->uponSignal("PrepareForSleep").onInterface(kLogindManagerInterface).call([this](bool sleeping) {
    if (m_prepareForSleepCallback) {
      m_prepareForSleepCallback(sleeping);
    }
  });
}

void LogindService::setPrepareForSleepCallback(PrepareForSleepCallback callback) {
  m_prepareForSleepCallback = std::move(callback);
}
