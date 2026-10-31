#include "core/deferred_call.h"
#include "core/timer_manager.h"
#include "dbus/session_bus.h"
#include "dbus/tray/tray_service.h"
#include "tests/test_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <vector>

namespace {
  constexpr auto kWatcherInterface = "org.kde.StatusNotifierWatcher";
  constexpr auto kItemInterface = "org.kde.StatusNotifierItem";

  void runDeferredCalls() {
    for (auto& callback : DeferredCall::takePending()) {
      callback();
    }
  }

  template <typename Predicate> bool drainUntil(SessionBus& bus, Predicate&& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      runDeferredCalls();
      TimerManager::instance().tick();
      if (std::invoke(ready)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::invoke(ready);
  }
} // namespace

int main(int argc, char** argv) {
  TEST_CHECK(argc == 3);
  TEST_CHECK(
      std::string{argv[1]} == "discovery"
      || std::string{argv[1]} == "registration"
      || std::string{argv[1]} == "external-watcher"
  );
  const bool discovery = std::string{argv[1]} == "discovery";
  const bool externalWatcher = std::string{argv[1]} == "external-watcher";
  const std::string scenario{argv[2]};
  TEST_CHECK(scenario == "default" || scenario == "ayatana" || scenario == "both");
  const std::size_t expectedCount = scenario == "both" ? 2 : 1;
  auto itemConnection = sdbus::createSessionBusConnection();
  const sdbus::ServiceName itemName{"org.kde.StatusNotifierItem-4242-1"};
  if (discovery || externalWatcher) {
    itemConnection->requestName(itemName);
  }
  auto createItem = [&](const std::string& path, bool hasProperties) {
    auto item = sdbus::createObject(*itemConnection, sdbus::ObjectPath{path});
    if (hasProperties) {
      item->addVTable(
              sdbus::registerProperty("IconName").withGetter([]() { return std::string{"test-tray-icon"}; })
      ).forInterface(kItemInterface);
    } else {
      // An interface without properties returns a successful, empty GetAll reply.
      item->addVTable(sdbus::registerSignal("NewIcon").withParameters<>()).forInterface(kItemInterface);
    }
    return item;
  };
  auto defaultItem = createItem("/StatusNotifierItem", scenario != "ayatana");
  auto ayatanaItem = createItem("/org/ayatana/NotificationItem", scenario != "default");
  itemConnection->enterEventLoopAsync();

  std::unique_ptr<sdbus::IConnection> externalConnection;
  std::unique_ptr<sdbus::IObject> externalObject;
  if (externalWatcher) {
    externalConnection = sdbus::createSessionBusConnection(sdbus::ServiceName{kWatcherInterface});
    externalObject = sdbus::createObject(*externalConnection, sdbus::ObjectPath{"/StatusNotifierWatcher"});
    externalObject
        ->addVTable(
            sdbus::registerMethod("RegisterStatusNotifierHost").implementedAs([](const std::string&) {}),
            sdbus::registerProperty("RegisteredStatusNotifierItems").withGetter([&]() {
              std::vector<std::string> items;
              if (scenario != "ayatana") {
                items.push_back(std::string{itemName} + "/StatusNotifierItem");
              }
              if (scenario != "default") {
                items.push_back(std::string{itemName} + "/org/ayatana/NotificationItem");
              }
              return items;
            }),
            sdbus::registerSignal("StatusNotifierItemRegistered").withParameters<std::string>("service"),
            sdbus::registerSignal("StatusNotifierItemUnregistered").withParameters<std::string>("service")
        )
        .forInterface(kWatcherInterface);
    externalConnection->enterEventLoopAsync();
  }

  SessionBus bus;
  TrayService tray(bus);
  tray.start();
  runDeferredCalls();

  if (externalWatcher) {
    TEST_CHECK(drainUntil(bus, [&]() {
      const auto items = tray.items();
      return items.size() == expectedCount
          && std::ranges::all_of(items, [](const auto& item) { return item.iconName == "test-tray-icon"; });
    }));
    externalObject.reset();
    externalConnection->leaveEventLoop();
    externalConnection.reset();
    TEST_CHECK(drainUntil(bus, [&]() {
      auto daemon = sdbus::createProxy(
          *itemConnection, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
      );
      std::string owner;
      try {
        daemon->callMethod("GetNameOwner")
            .onInterface("org.freedesktop.DBus")
            .withTimeout(std::chrono::milliseconds(200))
            .withArguments(std::string{kWatcherInterface})
            .storeResultsTo(owner);
      } catch (const sdbus::Error&) {
        return false;
      }
      return owner == bus.connection().getUniqueName() && tray.itemCount() == expectedCount;
    }));
    itemConnection->leaveEventLoop();
    return 0;
  }

  auto watcher = sdbus::createProxy(
      *itemConnection, sdbus::ServiceName{kWatcherInterface}, sdbus::ObjectPath{"/StatusNotifierWatcher"}
  );
  std::atomic<bool> acknowledged = false;
  std::atomic<bool> registrationFailed = false;
  const std::string uniqueName = itemConnection->getUniqueName();
  auto registerItem = [&]() {
    acknowledged = false;
    watcher->callMethodAsync("RegisterStatusNotifierItem")
        .onInterface(kWatcherInterface)
        .withArguments(uniqueName)
        // sdbus-c++ recognizes the error parameter only when it is passed by value.
        .uponReplyInvoke([&](std::optional<sdbus::Error> error) { // NOLINT(performance-unnecessary-value-param)
          registrationFailed = error.has_value();
          acknowledged = true;
        });
    // Stop dispatching once the watcher has queued this registration, leaving
    // any owner-check reply pending while the simulated startup frame runs.
    std::vector<std::function<void()>> registrationCalls;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (registrationCalls.empty() && std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      registrationCalls = DeferredCall::takePending();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(!registrationCalls.empty());
    for (auto& callback : registrationCalls) {
      callback();
    }
    TimerManager::instance().tick();
    while (!acknowledged && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(acknowledged);
    TEST_CHECK(!registrationFailed);
  };

  if (!discovery) {
    registerItem();
  }

  // A busy startup frame can delay dispatch even when the bus daemon replied
  // immediately. An acknowledged item and startup discovery must survive this.
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  TEST_CHECK(drainUntil(bus, [&]() {
    const auto items = tray.items();
    return items.size() == expectedCount
        && std::ranges::all_of(items, [](const auto& item) { return item.iconName == "test-tray-icon"; });
  }));

  if (!discovery) {
    registerItem();
    TEST_CHECK(drainUntil(bus, [&]() { return tray.itemCount() == expectedCount; }));
  }
  TEST_CHECK(tray.registeredItems().size() == expectedCount);
  for (const auto& item : tray.items()) {
    TEST_CHECK(item.busName == (discovery ? std::string{itemName} : uniqueName));
    if (scenario != "both") {
      TEST_CHECK(item.objectPath == (scenario == "default" ? "/StatusNotifierItem" : "/org/ayatana/NotificationItem"));
    }
  }
  itemConnection->leaveEventLoop();
}
