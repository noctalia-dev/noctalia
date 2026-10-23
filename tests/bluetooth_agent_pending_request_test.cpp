#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/system_bus.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <sdbus-c++/sdbus-c++.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// Guards the precondition the control-center-popup fix relies on: BlueZ's
// Agent1 callback must fire for every pairing request, unconditionally, and
// the request must stay pending until something explicitly answers it --
// nothing in BluetoothAgent auto-accepts. If that ever changed (e.g. a future
// "helpful" auto-confirm), the application-level fix that opens the control
// center on an unanswered request would stop being necessary, or worse, could
// race it.
int main() {
  auto connection = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.bluez"});
  auto root = sdbus::createObject(*connection, sdbus::ObjectPath{"/org/bluez"});

  sdbus::ObjectPath registeredAgentPath;
  bool defaultAgentRequested = false;

  root->addVTable(
          sdbus::registerMethod("RegisterAgent").implementedAs([&](sdbus::ObjectPath path, std::string /*capability*/) {
            registeredAgentPath = path;
          }),
          sdbus::registerMethod("RequestDefaultAgent").implementedAs([&](sdbus::ObjectPath /*path*/) {
            defaultAgentRequested = true;
          }),
          sdbus::registerMethod("UnregisterAgent").implementedAs([](sdbus::ObjectPath /*path*/) {})
  ).forInterface("org.bluez.AgentManager1");
  connection->enterEventLoopAsync();

  SystemBus bus;
  BluetoothAgent agent(bus);

  auto wait = [&](const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      std::this_thread::sleep_for(1ms);
    }
    TEST_CHECK(ready());
  };

  wait([&] { return defaultAgentRequested; });
  TEST_CHECK(registeredAgentPath == sdbus::ObjectPath{"/org/noctalia/BluetoothAgent"});

  std::vector<BluetoothPairingKind> seenKinds;
  agent.setRequestCallback([&](const BluetoothPairingRequest& req) { seenKinds.push_back(req.kind); });

  // Simulate BlueZ asking for confirmation, exactly like the real bluetoothd
  // does while waiting on the registered Agent1. This call must not return on
  // its own -- it blocks until acceptConfirm()/rejectConfirm() replies.
  auto agentProxy = sdbus::createProxy(
      *connection, sdbus::ServiceName{bus.connection().getUniqueName()},
      sdbus::ObjectPath{"/org/noctalia/BluetoothAgent"}
  );
  std::atomic<bool> confirmReturned{false};
  std::thread confirmCaller([&] {
    agentProxy->callMethod("RequestConfirmation")
        .onInterface("org.bluez.Agent1")
        .withArguments(sdbus::ObjectPath{"/org/bluez/hci0/dev_00_11_22_33_44_55"}, std::uint32_t{123456});
    confirmReturned = true;
  });

  wait([&] { return !seenKinds.empty(); });
  TEST_CHECK(seenKinds.back() == BluetoothPairingKind::Confirm);
  TEST_CHECK(agent.hasPendingRequest());
  TEST_CHECK(agent.pendingRequest().passkey == 123456);

  // Nothing in BluetoothAgent answers on its own: give it time to prove that,
  // instead of racing a single poll iteration.
  std::this_thread::sleep_for(300ms);
  bus.processPendingEvents();
  TEST_CHECK(!confirmReturned);
  TEST_CHECK(agent.hasPendingRequest());

  agent.acceptConfirm();
  wait([&] { return confirmReturned.load(); });
  confirmCaller.join();
  TEST_CHECK(!agent.hasPendingRequest());

  connection->leaveEventLoop();
}
