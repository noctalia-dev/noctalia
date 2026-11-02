#include "core/deferred_call.h"
#include "core/timer_manager.h"
#include "dbus/session_bus.h"
#include "dbus/tray/tray_service.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
  constexpr auto kItemInterface = "org.kde.StatusNotifierItem";
  constexpr auto kItemPath = "/StatusNotifierItem";

  void runDeferredCalls() {
    for (auto& callback : DeferredCall::takePending()) {
      callback();
    }
  }

  void drainOnce(SessionBus& bus) {
    bus.processPendingEvents();
    runDeferredCalls();
    TimerManager::instance().tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  template <typename Predicate> bool drainUntil(SessionBus& bus, Predicate&& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      drainOnce(bus);
      if (std::invoke(ready)) {
        return true;
      }
    }
    return std::invoke(ready);
  }

  void drainFor(SessionBus& bus, std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
      drainOnce(bus);
    }
  }

  std::string iconNameOf(const TrayService& tray, const std::string& busName) {
    for (const auto& item : tray.items()) {
      if (item.busName == busName) {
        return item.iconName;
      }
    }
    return {};
  }

  // Serves an item that switches its icon and emits NewIcon for every byte read from `commands`.
  // The tray drops same-process items on the same path as duplicates, so it runs in its own process.
  [[noreturn]] void runEmittingItem(const std::string& busName, int commands) {
    auto connection = sdbus::createSessionBusConnection();
    connection->requestName(sdbus::ServiceName{busName});
    auto object = sdbus::createObject(*connection, sdbus::ObjectPath{kItemPath});
    std::mutex iconMutex;
    std::string iconName = "emitter-icon";
    object
        ->addVTable(sdbus::registerProperty("IconName").withGetter([&]() {
          const std::scoped_lock lock(iconMutex);
          return iconName;
        }))
        .forInterface(kItemInterface);
    connection->enterEventLoopAsync();

    char command = 0;
    while (read(commands, &command, 1) == 1) {
      {
        const std::scoped_lock lock(iconMutex);
        iconName = "emitter-icon-changed";
      }
      object->emitSignal("NewIcon").onInterface(kItemInterface);
    }
    connection->leaveEventLoop();
    _exit(0);
  }
} // namespace

int main() {
  const std::string emitterName = "org.kde.StatusNotifierItem-1001-1";
  const std::string observerName = "org.kde.StatusNotifierItem-1002-1";

  int commandPipe[2];
  TEST_CHECK(pipe(commandPipe) == 0);
  const pid_t emitter = fork();
  TEST_CHECK(emitter >= 0);
  if (emitter == 0) {
    close(commandPipe[1]);
    runEmittingItem(emitterName, commandPipe[0]);
  }
  close(commandPipe[0]);

  // Items share the default object path and register by well-known name, as Electron and
  // libappindicator items do.
  auto observerConnection = sdbus::createSessionBusConnection();
  observerConnection->requestName(sdbus::ServiceName{observerName});
  auto observer = sdbus::createObject(*observerConnection, sdbus::ObjectPath{kItemPath});
  std::atomic<int> observerReads = 0;
  observer
      ->addVTable(sdbus::registerProperty("IconName").withGetter([&]() {
        ++observerReads;
        return std::string{"observer-icon"};
      }))
      .forInterface(kItemInterface);
  observerConnection->enterEventLoopAsync();

  SessionBus bus;
  TrayService tray(bus);
  tray.start();
  runDeferredCalls();

  TEST_CHECK(drainUntil(bus, [&]() {
    return iconNameOf(tray, emitterName) == "emitter-icon" && iconNameOf(tray, observerName) == "observer-icon";
  }));
  // Let registration-time metadata retries and owner lookups finish.
  drainFor(bus, std::chrono::milliseconds(1500));

  observerReads = 0;
  TEST_CHECK(write(commandPipe[1], "x", 1) == 1);
  TEST_CHECK(drainUntil(bus, [&]() { return iconNameOf(tray, emitterName) == "emitter-icon-changed"; }));
  drainFor(bus, std::chrono::milliseconds(300));
  TEST_CHECK(observerReads == 0);

  close(commandPipe[1]);
  int status = 0;
  TEST_CHECK(waitpid(emitter, &status, 0) == emitter);
  observerConnection->leaveEventLoop();
}
