#include "dbus/secret/secret_collection_probe.h"
#include "dbus/session_bus.h"
#include "dbus/tray/tray_service.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <vector>

namespace {

  const sdbus::ServiceName kSecretServiceBusName{"org.freedesktop.secrets"};
  const sdbus::ObjectPath kSecretServicePath{"/org/freedesktop/secrets"};
  constexpr auto kSecretServiceInterface = "org.freedesktop.Secret.Service";
  const sdbus::ObjectPath kSecretCollectionPath{"/org/freedesktop/secrets/collection/default"};
  constexpr auto kSecretCollectionInterface = "org.freedesktop.Secret.Collection";

  const sdbus::ServiceName kWatcherBusName{"org.kde.StatusNotifierWatcher"};
  const sdbus::ObjectPath kWatcherPath{"/StatusNotifierWatcher"};
  constexpr auto kWatcherInterface = "org.kde.StatusNotifierWatcher";
  constexpr auto kIntrospectableInterface = "org.freedesktop.DBus.Introspectable";

  class FakeSecretService {
  public:
    FakeSecretService()
        : m_connection(sdbus::createSessionBusConnection(kSecretServiceBusName)),
          m_serviceObject(sdbus::createObject(*m_connection, kSecretServicePath)),
          m_collectionObject(sdbus::createObject(*m_connection, kSecretCollectionPath)) {
      m_serviceObject
          ->addVTable(
              sdbus::registerMethod("ReadAlias")
                  .withInputParamNames("name")
                  .withOutputParamNames("collection")
                  .implementedAs([this](const std::string& alias) {
                    ++m_readAliasCalls;
                    if (alias != "default") {
                      m_validAlias.store(false);
                    }
                    introspectWatcher();
                    return kSecretCollectionPath;
                  })
          )
          .forInterface(kSecretServiceInterface);

      m_collectionObject
          ->addVTable(sdbus::registerProperty("Locked").withGetter([this]() {
            ++m_lockedReads;
            introspectWatcher();
            return false;
          }))
          .forInterface(kSecretCollectionInterface);

      m_connection->enterEventLoopAsync();
    }

    ~FakeSecretService() { m_connection->leaveEventLoop(); }

    FakeSecretService(const FakeSecretService&) = delete;
    FakeSecretService& operator=(const FakeSecretService&) = delete;

    [[nodiscard]] int readAliasCalls() const noexcept { return m_readAliasCalls.load(); }
    [[nodiscard]] int lockedReads() const noexcept { return m_lockedReads.load(); }
    [[nodiscard]] int introspectionAttempts() const noexcept { return m_introspectionAttempts.load(); }
    [[nodiscard]] int introspectionSuccesses() const noexcept { return m_introspectionSuccesses.load(); }
    [[nodiscard]] bool validAlias() const noexcept { return m_validAlias.load(); }

  private:
    void introspectWatcher() {
      ++m_introspectionAttempts;
      try {
        // KeePassXC performs this synchronous tray availability check from the
        // same thread that serves its Secret Service. Noctalia must remain able to
        // dispatch it while either Secret Service request is outstanding.
        auto client = sdbus::createSessionBusConnection();
        auto watcher = sdbus::createProxy(*client, kWatcherBusName, kWatcherPath);
        std::string xml;
        watcher->callMethod("Introspect")
            .onInterface(kIntrospectableInterface)
            .withTimeout(std::chrono::seconds(1))
            .storeResultsTo(xml);
        if (xml.contains(kWatcherInterface)) {
          ++m_introspectionSuccesses;
        }
      } catch (const sdbus::Error&) {
      }
    }

    std::unique_ptr<sdbus::IConnection> m_connection;
    std::unique_ptr<sdbus::IObject> m_serviceObject;
    std::unique_ptr<sdbus::IObject> m_collectionObject;
    std::atomic<int> m_readAliasCalls = 0;
    std::atomic<int> m_lockedReads = 0;
    std::atomic<int> m_introspectionAttempts = 0;
    std::atomic<int> m_introspectionSuccesses = 0;
    std::atomic<bool> m_validAlias = true;
  };

  template <typename Predicate>
  bool drainUntil(SessionBus& bus, Predicate&& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      if (std::invoke(predicate)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bus.processPendingEvents();
    return std::invoke(predicate);
  }

} // namespace

int main() {
  FakeSecretService secretService;
  SessionBus bus;
  TrayService tray(bus);
  tray.start();

  std::vector<bool> results;
  SecretCollectionProbe probe(bus, [&results](bool unlocked) { results.push_back(unlocked); });

  const auto requestStart = std::chrono::steady_clock::now();
  probe.request();
  const auto requestElapsed = std::chrono::steady_clock::now() - requestStart;

  // A synchronous ReadAlias implementation blocks here until the fake service's
  // nested Introspect times out. The production probe must only enqueue work.
  TEST_CHECK(requestElapsed < std::chrono::milliseconds(500));
  TEST_CHECK(drainUntil(bus, [&results]() { return results.size() == 1; }, std::chrono::seconds(3)));

  TEST_CHECK(secretService.validAlias());
  TEST_CHECK(secretService.readAliasCalls() == 1);
  TEST_CHECK(secretService.lockedReads() == 1);
  TEST_CHECK(secretService.introspectionAttempts() == 2);
  TEST_CHECK(secretService.introspectionSuccesses() == 2);
  TEST_CHECK(results.front());

  // Multiple requests while one is outstanding coalesce to one pending rerun.
  probe.request();
  probe.request();
  probe.request();
  TEST_CHECK(drainUntil(bus, [&results]() { return results.size() == 3; }, std::chrono::seconds(3)));
  TEST_CHECK(secretService.readAliasCalls() == 3);
  TEST_CHECK(secretService.lockedReads() == 3);
  TEST_CHECK(secretService.introspectionAttempts() == 6);
  TEST_CHECK(secretService.introspectionSuccesses() == 6);
  TEST_CHECK(results[1]);
  TEST_CHECK(results[2]);

  // Invalidating an owner generation suppresses its outstanding reply without
  // suppressing a request issued for the replacement owner.
  probe.request();
  probe.invalidate();
  probe.request();
  TEST_CHECK(drainUntil(
      bus,
      [&]() { return results.size() == 4 && secretService.readAliasCalls() == 5 && secretService.lockedReads() == 4; },
      std::chrono::seconds(3)
  ));
  for (int attempt = 0; attempt < 20; ++attempt) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  TEST_CHECK(results.size() == 4);
  TEST_CHECK(secretService.introspectionAttempts() == 9);
  TEST_CHECK(secretService.introspectionSuccesses() == 9);
  TEST_CHECK(results[3]);
}
