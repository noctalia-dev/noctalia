#include "core/timer_manager.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/network_service.h"
#include "dbus/system_bus.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

  constexpr auto kNmBusName = "org.freedesktop.NetworkManager";

  class FallbackService final : public INetworkService {
  public:
    FallbackService() { m_state.ssid = "fallback"; }

    void setChangeCallback(ChangeCallback callback) override { m_changeCallback = std::move(callback); }
    [[nodiscard]] bool available() const noexcept override { return m_available; }
    void notify() {
      if (m_changeCallback) {
        m_changeCallback(m_state, NetworkChangeOrigin::External);
      }
    }
    void refresh() override {
      if (m_changeCallback) {
        m_changeCallback(m_state, NetworkChangeOrigin::External);
      }
    }
    [[nodiscard]] const NetworkState& state() const noexcept override { return m_state; }
    [[nodiscard]] bool hasStateSnapshot() const noexcept override { return true; }
    [[nodiscard]] const std::vector<AccessPointInfo>& accessPoints() const noexcept override { return m_accessPoints; }
    [[nodiscard]] const std::vector<VpnConnectionInfo>& vpnConnections() const noexcept override {
      return m_vpnConnections;
    }
    void requestScan() override { ++m_scanCount; }
    bool activateAccessPoint(const AccessPointInfo&) override { return true; }
    bool activateAccessPoint(const AccessPointInfo&, const std::string&) override { return true; }
    [[nodiscard]] bool supportsEnterprise() const noexcept override { return true; }
    bool
    activateEnterpriseAccessPoint(const AccessPointInfo&, const network_enterprise::EnterpriseCredentials&) override {
      return true;
    }
    bool activateVpnConnection(const VpnConnectionInfo&) override { return true; }
    bool deactivateVpnConnection(const VpnConnectionInfo&) override { return true; }
    [[nodiscard]] bool canActivateWiredConnection() const noexcept override { return true; }
    bool activateWiredConnection() override { return true; }
    [[nodiscard]] bool canActivateCellularConnection() const noexcept override { return true; }
    bool activateCellularConnection() override { return true; }
    bool deactivateCellularConnection() override { return true; }
    void setWirelessEnabled(bool enabled, WirelessEnabledCompletion completion) override {
      m_state.wirelessEnabled = enabled;
      if (m_holdCompletion) {
        m_pendingCompletion = std::move(completion);
      } else if (completion) {
        completion(true);
      }
    }
    void complete(bool success) {
      if (m_pendingCompletion) {
        m_pendingCompletion(success);
      }
    }
    void disconnect() override {}
    void forgetSsid(const std::string&) override {}
    [[nodiscard]] bool hasSavedConnection(const std::string&) const override { return true; }

    int m_scanCount = 0;
    bool m_available = true;
    bool m_holdCompletion = false;

  private:
    NetworkState m_state;
    std::vector<AccessPointInfo> m_accessPoints;
    std::vector<VpnConnectionInfo> m_vpnConnections;
    ChangeCallback m_changeCallback;
    WirelessEnabledCompletion m_pendingCompletion;
  };

  class FakeNetworkManager {
  public:
    FakeNetworkManager() : m_connection(sdbus::createSystemBusConnection()) {
      m_root = sdbus::createObject(*m_connection, sdbus::ObjectPath{"/org/freedesktop/NetworkManager"});
      m_root
          ->addVTable(
              sdbus::registerMethod("GetDevices").implementedAs([] { return std::vector<sdbus::ObjectPath>{}; }),
              sdbus::registerProperty("PrimaryConnection").withGetter([] { return sdbus::ObjectPath{"/"}; }),
              sdbus::registerProperty("ActiveConnections").withGetter([] { return std::vector<sdbus::ObjectPath>{}; }),
              sdbus::registerProperty("WirelessEnabled").withGetter([] { return true; })
          )
          .forInterface(kNmBusName);
      m_settings = sdbus::createObject(*m_connection, sdbus::ObjectPath{"/org/freedesktop/NetworkManager/Settings"});
      m_settings
          ->addVTable(sdbus::registerMethod("ListConnections").implementedAs([] {
            return std::vector<sdbus::ObjectPath>{};
          }))
          .forInterface("org.freedesktop.NetworkManager.Settings");
      m_connection->enterEventLoopAsync();
    }

    ~FakeNetworkManager() { m_connection->leaveEventLoop(); }

    void acquire() { m_connection->requestName(sdbus::ServiceName{kNmBusName}); }
    void release() { m_connection->releaseName(sdbus::ServiceName{kNmBusName}); }

    std::unique_ptr<sdbus::IConnection> m_connection;

  private:
    std::unique_ptr<sdbus::IObject> m_root;
    std::unique_ptr<sdbus::IObject> m_settings;
  };

  class PendingNetworkManagerService final : public NetworkManagerService {
  public:
    using NetworkManagerService::NetworkManagerService;

    [[nodiscard]] bool hasStateSnapshot() const noexcept override {
      return !m_hideSnapshot && NetworkManagerService::hasStateSnapshot();
    }

    void setWirelessEnabled(bool, WirelessEnabledCompletion completion) override {
      m_completion = std::move(completion);
    }

    void complete(bool success) {
      if (m_completion) {
        m_completion(success);
      }
    }

    bool m_hideSnapshot = false;

  private:
    WirelessEnabledCompletion m_completion;
  };

  void until(SystemBus& bus, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      TimerManager::instance().tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(done());
  }

  void testFallbackAndNetworkManager(SystemBus& bus, FakeNetworkManager& networkManager) {
    auto fallback = std::make_unique<FallbackService>();
    auto* original = fallback.get();
    NetworkService service(std::make_unique<NetworkManagerService>(bus), std::move(fallback));
    INetworkService* const stableService = &service;
    int backendChanges = 0;
    stableService->setChangeCallback([&](const NetworkState&, NetworkChangeOrigin origin) {
      if (origin == NetworkChangeOrigin::BackendChanged) {
        ++backendChanges;
      }
    });

    TEST_CHECK(stableService->state().ssid == "fallback");
    stableService->requestScan();
    TEST_CHECK(original->m_scanCount == 1);

    original->m_holdCompletion = true;
    int completionCount = 0;
    stableService->setWirelessEnabled(false, [&](bool success) {
      TEST_CHECK(!success);
      ++completionCount;
    });

    networkManager.acquire();
    until(bus, [&] { return stableService->state().ssid != "fallback" && stableService->hasStateSnapshot(); });
    TEST_CHECK(backendChanges == 1);
    TEST_CHECK(completionCount == 1);
    original->complete(true);
    TEST_CHECK(completionCount == 1);

    networkManager.release();
    until(bus, [&] { return !stableService->available(); });
    TEST_CHECK(backendChanges == 2);
    TEST_CHECK(stableService->state().ssid.empty());
    stableService->requestScan();
    TEST_CHECK(original->m_scanCount == 1);

    networkManager.acquire();
    until(bus, [&] { return stableService->available() && stableService->hasStateSnapshot(); });
    TEST_CHECK(backendChanges == 3);
    networkManager.release();
    until(bus, [&] { return !stableService->available(); });
    TEST_CHECK(backendChanges == 4);
  }

  void testWirelessCompletionOnOwnerLoss(SystemBus& bus, FakeNetworkManager& networkManager) {
    networkManager.acquire();
    auto initial = std::make_unique<PendingNetworkManagerService>(bus);
    auto* pending = initial.get();
    NetworkService service(std::move(initial), nullptr);
    TEST_CHECK(!service.hasStateSnapshot());

    int completionCount = 0;
    bool completionResult = true;
    service.setWirelessEnabled(false, [&](bool success) {
      ++completionCount;
      completionResult = success;
    });
    TEST_CHECK(completionCount == 0);

    networkManager.release();
    until(bus, [&] { return !service.available(); });
    TEST_CHECK(completionCount == 1);
    TEST_CHECK(!completionResult);
    pending->complete(true);
    TEST_CHECK(completionCount == 1);

    networkManager.acquire();
    until(bus, [&] { return service.available() && service.hasStateSnapshot(); });
    service.setWirelessEnabled(false, [&](bool success) {
      ++completionCount;
      completionResult = success;
    });
    networkManager.release();
    until(bus, [&] { return !service.available(); });
    TEST_CHECK(completionCount == 2);
    TEST_CHECK(!completionResult);
    pending->complete(true);
    TEST_CHECK(completionCount == 2);
  }

  void testOwnerLostBeforeFirstSnapshot(SystemBus& bus, FakeNetworkManager& networkManager) {
    auto networkManagerService = std::make_unique<PendingNetworkManagerService>(bus);
    networkManagerService->m_hideSnapshot = true;
    auto* networkManagerBackend = networkManagerService.get();
    NetworkService service(std::move(networkManagerService), std::make_unique<FallbackService>());
    int backendChanges = 0;
    service.setChangeCallback([&](const NetworkState&, NetworkChangeOrigin origin) {
      if (origin == NetworkChangeOrigin::BackendChanged) {
        ++backendChanges;
      }
    });

    networkManager.acquire();
    until(bus, [&] { return networkManagerBackend->available(); });
    TEST_CHECK(service.state().ssid == "fallback");
    networkManager.release();
    until(bus, [&] { return !networkManagerBackend->available(); });
    TEST_CHECK(service.state().ssid == "fallback");
    TEST_CHECK(backendChanges == 0);
  }

  void testUnobservedOwnerLossCancelsPendingRequest() {
    auto backend = std::make_unique<FallbackService>();
    backend->m_available = false;
    backend->m_holdCompletion = true;
    auto* pending = backend.get();
    NetworkService service(std::move(backend), nullptr);

    pending->m_available = true;
    TEST_CHECK(service.available());
    int completionCount = 0;
    service.setWirelessEnabled(false, [&](bool success) {
      TEST_CHECK(!success);
      ++completionCount;
    });
    pending->m_available = false;
    pending->notify();
    TEST_CHECK(completionCount == 1);
    pending->complete(true);
    TEST_CHECK(completionCount == 1);
  }

} // namespace

int main() {
  TEST_CHECK(std::getenv("DBUS_SYSTEM_BUS_ADDRESS") != nullptr);

  SystemBus bus;
  FakeNetworkManager networkManager;
  testFallbackAndNetworkManager(bus, networkManager);
  testOwnerLostBeforeFirstSnapshot(bus, networkManager);
  testUnobservedOwnerLossCancelsPendingRequest();
  testWirelessCompletionOnOwnerLoss(bus, networkManager);
  return 0;
}
