#include "core/timer_manager.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/system_bus.h"
#include "tests/test_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

  using Props = std::map<std::string, sdbus::Variant>;

  struct MockDevice {
    std::string path;
    bool trusted = true;
    bool connected = false;
    std::unique_ptr<sdbus::IObject> object;
  };

  // Connect() replies are held until the test releases them, so overlapping dispatches stay observable.
  struct ConnectLog {
    std::mutex mutex;
    std::vector<std::string> calls;
    std::vector<std::pair<std::string, sdbus::Result<>>> pending;
    std::size_t maxPending = 0;
  };

} // namespace

int main() {
  auto connection = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.bluez"});
  auto root = sdbus::createObject(*connection, sdbus::ObjectPath{"/"});
  root->addObjectManager();
  auto adapter = sdbus::createObject(*connection, sdbus::ObjectPath{"/org/bluez/hci0"});
  adapter->addVTable(
             sdbus::registerProperty("Powered").withGetter([] { return true; })
  ).forInterface("org.bluez.Adapter1");

  ConnectLog log;
  // a/b dial; c is untrusted; d is already connected; e reconnects on its own mid-pass.
  std::array<MockDevice, 5> devices{{
      {.path = "/org/bluez/hci0/dev_a"},
      {.path = "/org/bluez/hci0/dev_b"},
      {.path = "/org/bluez/hci0/dev_c", .trusted = false},
      {.path = "/org/bluez/hci0/dev_d", .connected = true},
      {.path = "/org/bluez/hci0/dev_e"},
  }};
  std::mutex deviceMutex;
  for (auto& dev : devices) {
    dev.object = sdbus::createObject(*connection, sdbus::ObjectPath{dev.path});
    const std::string path = dev.path;
    dev.object
        ->addVTable(
            sdbus::registerProperty("Address").withGetter([path] { return path.substr(path.size() - 1); }),
            sdbus::registerProperty("Paired").withGetter([] { return true; }),
            sdbus::registerProperty("Trusted").withGetter([&dev, &deviceMutex] {
              const std::scoped_lock lock(deviceMutex);
              return dev.trusted;
            }),
            sdbus::registerProperty("Connected").withGetter([&dev, &deviceMutex] {
              const std::scoped_lock lock(deviceMutex);
              return dev.connected;
            }),
            sdbus::registerMethod("Connect").implementedAs([&log, path](sdbus::Result<>&& result) {
              const std::scoped_lock lock(log.mutex);
              log.calls.push_back(path);
              log.pending.emplace_back(path, std::move(result));
              log.maxPending = std::max(log.maxPending, log.pending.size());
            })
        )
        .forInterface("org.bluez.Device1");
  }
  connection->enterEventLoopAsync();

  SystemBus bus;
  BluetoothService service(bus, nullptr);
  auto pump = [&](std::chrono::milliseconds duration, const std::function<bool()>& done = {}) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      TimerManager::instance().tick();
      if (done && done()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    return false;
  };
  auto callCount = [&] {
    const std::scoped_lock lock(log.mutex);
    return log.calls.size();
  };
  auto pendingCount = [&] {
    const std::scoped_lock lock(log.mutex);
    return log.pending.size();
  };
  auto setConnected = [&](MockDevice& dev) {
    {
      const std::scoped_lock lock(deviceMutex);
      dev.connected = true;
    }
    dev.object->emitSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .withArguments(
            std::string{"org.bluez.Device1"}, Props{{"Connected", sdbus::Variant{true}}}, std::vector<std::string>{}
        );
  };
  auto emitPowered = [&](bool powered) {
    adapter->emitSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .withArguments(
            std::string{"org.bluez.Adapter1"}, Props{{"Powered", sdbus::Variant{powered}}}, std::vector<std::string>{}
        );
    TEST_CHECK(pump(5s, [&] { return service.state().powered == powered; }));
  };
  auto isConnecting = [&](const std::string& path) {
    const auto& list = service.devices();
    const auto it = std::ranges::find(list, path, &BluetoothDeviceInfo::path);
    return it != list.end() && it->connecting;
  };

  TEST_CHECK(pump(5s, [&] { return service.hasStateSnapshot() && service.devices().size() == devices.size(); }));

  // Off by default: startup with a powered adapter dials nothing.
  pump(3s);
  TEST_CHECK(callCount() == 0);

  service.setConnectWhenEnabled(true);
  emitPowered(false);
  emitPowered(true);

  TEST_CHECK(pump(5s, [&] { return pendingCount() == 1; }));
  std::string first;
  {
    const std::scoped_lock lock(log.mutex);
    first = log.pending.front().first;
  }
  TEST_CHECK(first == devices[0].path);
  // The next device waits for this reply, and the resume re-syncs neither restart the pass nor
  // drop the in-flight marker.
  service.refresh();
  service.refresh();
  pump(3s);
  TEST_CHECK(callCount() == 1);
  TEST_CHECK(isConnecting(first));

  // e comes up by itself while the queue is busy, so the pass must skip it.
  setConnected(devices[4]);
  TEST_CHECK(pump(5s, [&] {
    const auto& list = service.devices();
    const auto it = std::ranges::find(list, devices[4].path, &BluetoothDeviceInfo::path);
    return it != list.end() && it->connected;
  }));

  // Release replies one at a time: a failure for the first device, success for the second.
  while (true) {
    std::optional<std::pair<std::string, sdbus::Result<>>> reply;
    {
      const std::scoped_lock lock(log.mutex);
      if (!log.pending.empty()) {
        reply.emplace(std::move(log.pending.front()));
        log.pending.erase(log.pending.begin());
      }
    }
    if (!reply.has_value()) {
      break;
    }
    if (reply->first == first) {
      reply->second.returnError(sdbus::Error(sdbus::Error::Name{"org.bluez.Error.Failed"}, "page-timeout"));
    } else {
      auto it = std::ranges::find(devices, reply->first, &MockDevice::path);
      TEST_CHECK(it != devices.end());
      setConnected(*it);
      reply->second.returnResults();
    }
    pump(1s, [&] { return pendingCount() > 0; });
  }
  pump(1s);

  std::vector<std::string> calls;
  std::size_t maxPending = 0;
  {
    const std::scoped_lock lock(log.mutex);
    calls = log.calls;
    maxPending = log.maxPending;
  }
  std::ranges::sort(calls);
  TEST_CHECK((calls == std::vector<std::string>{devices[0].path, devices[1].path}));
  TEST_CHECK(maxPending == 1);
  TEST_CHECK(!isConnecting(first));

  connection->leaveEventLoop();
}
