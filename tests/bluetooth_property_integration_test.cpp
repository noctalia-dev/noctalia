#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/system_bus.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <semaphore>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
  using Props = std::map<std::string, sdbus::Variant>;
  auto connection = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.bluez"});
  auto root = sdbus::createObject(*connection, sdbus::ObjectPath{"/"});
  root->addObjectManager();
  auto adapter = sdbus::createObject(*connection, sdbus::ObjectPath{"/org/bluez/hci0"});
  const sdbus::ObjectPath devicePath{"/org/bluez/hci0/dev_00_11_22_33_44_55"};
  auto device = sdbus::createObject(*connection, devicePath);
  std::atomic<int> writes = 0;
  std::counting_semaphore<7> replies{0};
  auto delayedSetter = [&](bool) {
    const bool released = replies.try_acquire_for(10s);
    assert(released && "Property dispatch waited for a withheld reply");
    const int request = ++writes;
    if (request == 5 || request == 7) {
      throw sdbus::Error(sdbus::Error::Name{"org.bluez.Error.Failed"}, "Test property failure");
    }
  };
  adapter
      ->addVTable(
          sdbus::registerProperty("Powered").withGetter([] { return true; }).withSetter(delayedSetter),
          sdbus::registerProperty("Discoverable").withGetter([] { return false; }).withSetter(delayedSetter),
          sdbus::registerProperty("Pairable").withGetter([] { return true; }).withSetter(delayedSetter)
      )
      .forInterface("org.bluez.Adapter1");
  device
      ->addVTable(
          sdbus::registerProperty("Address").withGetter([] { return std::string{"00:11:22:33:44:55"}; }),
          sdbus::registerProperty("Trusted").withGetter([] { return false; }).withSetter(delayedSetter)
      )
      .forInterface("org.bluez.Device1");
  connection->enterEventLoopAsync();
  SystemBus bus;
  BluetoothService service(bus, nullptr);
  auto wait = [&](const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      std::this_thread::sleep_for(1ms);
    }
    assert(ready());
  };
  wait([&] { return service.hasStateSnapshot() && service.devices().size() == 1; });

  // Replies stay withheld until all four setters have returned. The server timeout
  // only guards against deadlock if a setter becomes synchronous again.
  service.setPowered(false);
  service.setDiscoverable(true);
  service.setPairable(false);
  service.setTrusted(devicePath, true);
  assert(writes == 0);
  replies.release(4);
  assert(service.state().powered);
  wait([&] { return writes == 4; });

  int stateChanges = 0;
  auto lastOrigin = BluetoothStateChangeOrigin::External;
  service.setStateCallback([&](const BluetoothState&, BluetoothStateChangeOrigin origin) {
    ++stateChanges;
    lastOrigin = origin;
  });
  auto emitPowered = [&](bool powered) {
    adapter->emitSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .withArguments(
            std::string{"org.bluez.Adapter1"}, Props{{"Powered", sdbus::Variant{powered}}}, std::vector<std::string>{}
        );
    wait([&] { return service.state().powered == powered; });
  };
  emitPowered(false);
  assert(lastOrigin == BluetoothStateChangeOrigin::Noctalia);
  emitPowered(true);
  assert(lastOrigin == BluetoothStateChangeOrigin::External);

  // An earlier failure must not clear a newer request for the same power state.
  const int beforeRepeatedRequest = stateChanges;
  service.setPowered(false);
  service.setPowered(false);
  replies.release();
  wait([&] { return writes == 5 && stateChanges > beforeRepeatedRequest; });
  replies.release();
  wait([&] { return writes == 6; });
  emitPowered(false);
  assert(lastOrigin == BluetoothStateChangeOrigin::Noctalia);
  emitPowered(true);

  // A failed local request must not mislabel the next external power change.
  const int previousChanges = stateChanges;
  service.setPowered(false);
  replies.release();
  wait([&] { return writes == 7 && stateChanges > previousChanges; });
  assert(service.state().powered);
  emitPowered(false);
  assert(lastOrigin == BluetoothStateChangeOrigin::External);
  connection->leaveEventLoop();
}
