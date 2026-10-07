#include "core/deferred_call.h"
#include "core/timer_manager.h"
#include "dbus/session_bus.h"
#include "dbus/tray/tray_service.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <functional>
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
  TEST_CHECK(argc == 2);
  TEST_CHECK(std::string{argv[1]} == "discovery" || std::string{argv[1]} == "registration");
  const bool discovery = std::string{argv[1]} == "discovery";
  auto itemConnection = sdbus::createSessionBusConnection();
  const sdbus::ServiceName itemName{"org.kde.StatusNotifierItem-4242-1"};
  if (discovery) {
    itemConnection->requestName(itemName);
  }
  auto item = sdbus::createObject(*itemConnection, sdbus::ObjectPath{"/StatusNotifierItem"});
  item->addVTable(
          sdbus::registerProperty("IconName").withGetter([]() { return std::string{"test-tray-icon"}; })
  ).forInterface(kItemInterface);
  itemConnection->enterEventLoopAsync();

  SessionBus bus;
  TrayService tray(bus);
  tray.start();
  runDeferredCalls();

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
        .uponReplyInvoke([&](std::optional<sdbus::Error> error) {
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
    return items.size() == 1 && items.front().iconName == "test-tray-icon";
  }));

  if (!discovery) {
    registerItem();
    TEST_CHECK(drainUntil(bus, [&]() { return tray.itemCount() == 1; }));
    TEST_CHECK(tray.items().front().busName == uniqueName);
  } else {
    TEST_CHECK(tray.items().front().busName == std::string{itemName});
  }
  itemConnection->leaveEventLoop();
}
