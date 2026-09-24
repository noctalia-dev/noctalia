#include "dbus/system_bus.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n_service.h"
#include "shell/control_center/tabs/power_tab.h"
#include "ui/controls/label.h"
#include "ui/controls/toggle.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

  const sdbus::ServiceName kBusName{"org.freedesktop.UPower"};
  const sdbus::ObjectPath kManagerPath{"/org/freedesktop/UPower"};
  constexpr auto kManagerInterface = "org.freedesktop.UPower";
  constexpr auto kDeviceInterface = "org.freedesktop.UPower.Device";
  class FakeBattery {
  public:
    FakeBattery(
        sdbus::IConnection& connection, std::string path, std::string nativePath, std::string model, bool powerSupply,
        bool registerChargeMethod, bool unregisterBeforeFirstIntrospection
    )
        : connection(connection), path(std::move(path)), nativePath(std::move(nativePath)), model(std::move(model)),
          powerSupply(powerSupply), chargeMethodRegistered(registerChargeMethod),
          unregisterBeforeFirstIntrospection(unregisterBeforeFirstIntrospection) {
      registerObject();
    }

    void registerObject() {
      object = sdbus::createObject(connection, sdbus::ObjectPath{path});
      object
          ->addVTable(
              sdbus::registerProperty("NativePath").withGetter([this]() { return this->nativePath; }),
              sdbus::registerProperty("Vendor").withGetter([]() { return std::string{"Noctalia Test"}; }),
              sdbus::registerProperty("Model").withGetter([this]() { return this->model; }),
              sdbus::registerProperty("Serial").withGetter([this]() { return this->nativePath; }),
              sdbus::registerProperty("Type").withGetter([]() { return std::uint32_t{2}; }),
              sdbus::registerProperty("PowerSupply").withGetter([this]() { return this->powerSupply; }),
              sdbus::registerProperty("EnergyFull").withGetter([]() { return 50.0; }),
              sdbus::registerProperty("EnergyFullDesign").withGetter([]() { return 55.0; }),
              sdbus::registerProperty("Percentage").withGetter([this]() {
                std::scoped_lock lock(stateMutex);
                ++percentageReads;
                return percentage;
              }),
              sdbus::registerProperty("IsPresent").withGetter([this]() { return present; }),
              sdbus::registerProperty("State").withGetter([]() { return std::uint32_t{2}; }),
              sdbus::registerProperty("TimeToEmpty").withGetter([]() { return std::int64_t{7200}; }),
              sdbus::registerProperty("TimeToFull").withGetter([]() { return std::int64_t{0}; }),
              sdbus::registerProperty("EnergyRate").withGetter([]() { return 8.0; }),
              sdbus::registerProperty("Energy").withGetter([]() { return 30.0; }),
              sdbus::registerProperty("ChargeThresholdSupported").withGetter([this]() {
                bool shouldUnregister = false;
                bool supported = false;
                {
                  std::scoped_lock lock(stateMutex);
                  ++supportedReads;
                  shouldUnregister = unregisterBeforeFirstIntrospection && supportedReads == 1;
                  supported = thresholdSupported;
                }
                if (shouldUnregister) {
                  object->unregister();
                }
                return supported;
              }),
              sdbus::registerProperty("ChargeThresholdEnabled").withGetter([this]() {
                std::scoped_lock lock(stateMutex);
                ++enabledReads;
                return thresholdEnabled;
              }),
              sdbus::registerProperty("ChargeThresholdSettingsSupported").withGetter([this]() {
                std::scoped_lock lock(stateMutex);
                return supportedSettings;
              }),
              sdbus::registerProperty("ChargeStartThreshold").withGetter([]() { return std::uint32_t{75}; }),
              sdbus::registerProperty("ChargeEndThreshold").withGetter([]() { return std::uint32_t{80}; })
          )
          .forInterface(kDeviceInterface);
      if (chargeMethodRegistered) {
        registerChargeMethodVTable();
      }
    }

    void registerChargeMethodVTable() {
      object
          ->addVTable(
              sdbus::registerMethod("EnableChargeThreshold")
                  .withInputParamNames("enabled")
                  .implementedAs([this](sdbus::Result<>&& result, bool /*enabled*/) {
                    std::scoped_lock lock(stateMutex);
                    pendingResults.push_back(std::move(result));
                  })
          )
          .forInterface(kDeviceInterface);
    }

    void reappearWithChargeMethod() {
      object.reset();
      chargeMethodRegistered = true;
      unregisterBeforeFirstIntrospection = false;
      registerObject();
    }

    void exposeChargeMethod() {
      assert(!chargeMethodRegistered);
      chargeMethodRegistered = true;
      registerChargeMethodVTable();
    }

    void disableThresholdSupport() {
      std::scoped_lock lock(stateMutex);
      thresholdSupported = false;
      supportedSettings = 0;
    }

    void completeSuccess(bool enabled) {
      std::optional<sdbus::Result<>> result;
      {
        std::scoped_lock lock(stateMutex);
        assert(!pendingResults.empty());
        thresholdEnabled = enabled;
        result.emplace(std::move(pendingResults.front()));
        pendingResults.erase(pendingResults.begin());
      }
      result->returnResults();
    }

    void completeFailure(std::string message) {
      std::optional<sdbus::Result<>> result;
      {
        std::scoped_lock lock(stateMutex);
        assert(!pendingResults.empty());
        result.emplace(std::move(pendingResults.front()));
        pendingResults.erase(pendingResults.begin());
      }
      result->returnError(sdbus::Error{sdbus::Error::Name{"org.freedesktop.UPower.GeneralError"}, std::move(message)});
    }

    void emitChanged() { object->emitPropertiesChangedSignal(kDeviceInterface); }
    void emitChanged(std::string property) {
      object->emitPropertiesChangedSignal(
          kDeviceInterface, std::vector<sdbus::PropertyName>{sdbus::PropertyName{std::move(property)}}
      );
    }
    void setPercentage(double value) {
      std::scoped_lock lock(stateMutex);
      percentage = value;
    }
    std::size_t pendingCount() const {
      std::scoped_lock lock(stateMutex);
      return pendingResults.size();
    }
    int supportedReadCount() const {
      std::scoped_lock lock(stateMutex);
      return supportedReads;
    }
    int enabledReadCount() const {
      std::scoped_lock lock(stateMutex);
      return enabledReads;
    }
    int percentageReadCount() const {
      std::scoped_lock lock(stateMutex);
      return percentageReads;
    }
    sdbus::IConnection& connection;
    std::string path;
    std::string nativePath;
    std::string model;
    bool powerSupply = true;
    bool present = true;
    mutable std::mutex stateMutex;
    bool chargeMethodRegistered = true;
    bool unregisterBeforeFirstIntrospection = false;
    bool thresholdSupported = true;
    bool thresholdEnabled = false;
    std::uint32_t supportedSettings = 3;
    double percentage = 55.0;
    int supportedReads = 0;
    int enabledReads = 0;
    int percentageReads = 0;
    std::vector<sdbus::Result<>> pendingResults;
    std::unique_ptr<sdbus::IObject> object;
  };

  class FakeUPower {
  public:
    FakeUPower()
        : connection(sdbus::createSessionBusConnection(kBusName)),
          manager(sdbus::createObject(*connection, kManagerPath)) {
      manager
          ->addVTable(
              sdbus::registerMethod("EnumerateDevices").implementedAs([this]() {
                std::scoped_lock lock(pathsMutex);
                return paths;
              }),
              sdbus::registerMethod("GetDisplayDevice").implementedAs([]() { return sdbus::ObjectPath{"/"}; }),
              sdbus::registerSignal("DeviceAdded").withParameters<sdbus::ObjectPath>("device"),
              sdbus::registerSignal("DeviceRemoved").withParameters<sdbus::ObjectPath>("device"),
              sdbus::registerProperty("OnBattery").withGetter([]() { return false; })
          )
          .forInterface(kManagerInterface);
      connection->enterEventLoopAsync();
    }

    ~FakeUPower() { connection->leaveEventLoop(); }

    FakeBattery& addBattery(
        std::string id, std::string model, bool emitSignal = false, bool powerSupply = true,
        bool registerChargeMethod = true, bool unregisterBeforeFirstIntrospection = false
    ) {
      const std::string path = "/org/freedesktop/UPower/devices/battery_" + id;
      batteries.push_back(
          std::make_unique<FakeBattery>(
              *connection, path, "NOCTALIA_TEST_" + id, std::move(model), powerSupply, registerChargeMethod,
              unregisterBeforeFirstIntrospection
          )
      );
      {
        std::scoped_lock lock(pathsMutex);
        paths.emplace_back(path);
      }
      if (emitSignal) {
        manager->emitSignal("DeviceAdded").onInterface(kManagerInterface).withArguments(sdbus::ObjectPath{path});
      }
      return *batteries.back();
    }

    void removeBattery(FakeBattery& battery) {
      {
        std::scoped_lock lock(pathsMutex);
        std::erase(paths, sdbus::ObjectPath{battery.path});
      }
      manager->emitSignal("DeviceRemoved")
          .onInterface(kManagerInterface)
          .withArguments(sdbus::ObjectPath{battery.path});
    }

    void readdBattery(FakeBattery& battery) {
      {
        std::scoped_lock lock(pathsMutex);
        paths.emplace_back(battery.path);
      }
      manager->emitSignal("DeviceAdded").onInterface(kManagerInterface).withArguments(sdbus::ObjectPath{battery.path});
    }

    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> manager;
    std::mutex pathsMutex;
    std::vector<sdbus::ObjectPath> paths;
    std::vector<std::unique_ptr<FakeBattery>> batteries;
  };

  template <typename Predicate> void drainUntil(SystemBus& bus, Predicate&& predicate) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
      bus.processPendingEvents();
      if (std::invoke(predicate)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "timed out waiting for D-Bus state");
  }

  const UPowerDeviceInfo* findBattery(const UPowerService& service, const std::string& path) {
    for (const auto& battery : service.batteryDevices()) {
      if (battery.path == path) {
        return service.deviceForSelector(path);
      }
    }
    return nullptr;
  }

} // namespace

class PowerTabTestAccess {
public:
  static void rebuild(PowerTab& tab) { tab.rebuildChargeLimits(); }
  static std::size_t rowCount(const PowerTab& tab) { return tab.m_chargeLimitRows.size(); }
  static bool nameVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).nameLabel->visible();
  }
  static std::string name(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).nameLabel->text();
  }
  static bool toggleChecked(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).toggle->checked();
  }
  static bool toggleEnabled(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).toggle->enabled();
  }
  static bool errorVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).errorLabel->visible();
  }
  static std::string error(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).errorLabel->text();
  }
  static std::string behavior(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).behaviorLabel->text();
  }
  static bool configuredVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).configuredLabel->visible();
  }
  static bool managementVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).managementLabel->visible();
  }
  static bool controlVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).controlRow->visible();
  }
  static float behaviorOpacity(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).behaviorLabel->color().a;
  }
};

int main() {
  i18n::Service::instance().init("en");
  FakeUPower fake;
  auto& bat0 = fake.addBattery("BAT0", "Primary Battery");
  auto& bat1 = fake.addBattery("BAT1", "Secondary Battery");
  auto& mouse = fake.addBattery("MOUSE0", "Wireless Mouse", false, false);
  auto& retryBattery = fake.addBattery("RETRY", "Retry Battery", false, true, true, true);
  auto& unsupportedMethod = fake.addBattery("NO_METHOD", "No Method Battery", false, true, false);

  {
    SystemBus capabilityBus;
    UPowerService capabilityService(capabilityBus);
    auto* retryInfo = findBattery(capabilityService, retryBattery.path);
    assert(retryInfo != nullptr && !retryInfo->chargeLimit.methodAvailable);
    retryBattery.reappearWithChargeMethod();
    retryBattery.emitChanged();
    drainUntil(capabilityBus, [&]() {
      const auto* current = findBattery(capabilityService, retryBattery.path);
      return current != nullptr && current->chargeLimit.methodAvailable;
    });
    const auto* unsupportedInfo = findBattery(capabilityService, unsupportedMethod.path);
    assert(unsupportedInfo != nullptr && !unsupportedInfo->chargeLimit.methodAvailable);
    unsupportedMethod.exposeChargeMethod();
    unsupportedMethod.emitChanged();
    for (int i = 0; i < 20; ++i) {
      capabilityBus.processPendingEvents();
    }
    unsupportedInfo = findBattery(capabilityService, unsupportedMethod.path);
    assert(unsupportedInfo != nullptr && !unsupportedInfo->chargeLimit.methodAvailable);
  }
  fake.removeBattery(retryBattery);
  fake.removeBattery(unsupportedMethod);
  auto& hiddenBattery = fake.addBattery("UNSUPPORTED", "Unsupported Battery");
  hiddenBattery.disableThresholdSupport();

  SystemBus bus;
  {
    UPowerService service(bus);
    int deviceChanges = 0;
    int chargeLimitChanges = 0;
    service.setChangeCallback([&](const UPowerChange& change) {
      if (change.origin == UPowerService::ChangeOrigin::DeviceState) {
        ++deviceChanges;
      } else {
        ++chargeLimitChanges;
      }
    });

    PowerTab tab(&service, nullptr);
    auto root = tab.create();
    PowerTabTestAccess::rebuild(tab);
    assert(PowerTabTestAccess::rowCount(tab) == 2);
    assert(PowerTabTestAccess::nameVisible(tab, 0));
    assert(PowerTabTestAccess::nameVisible(tab, 1));
    assert(PowerTabTestAccess::name(tab, 0) == "Primary Battery");
    assert(PowerTabTestAccess::name(tab, 1) == "Secondary Battery");
    assert(findBattery(service, hiddenBattery.path) != nullptr);
    assert(PowerTabTestAccess::behavior(tab, 0) == "Can charge to 100%");
    assert(PowerTabTestAccess::behaviorOpacity(tab, 0) == 1.0F);
    assert(!PowerTabTestAccess::configuredVisible(tab, 0));
    assert(!PowerTabTestAccess::managementVisible(tab, 0));
    assert(PowerTabTestAccess::controlVisible(tab, 0));
    assert(mouse.supportedReadCount() == 0);
    assert(mouse.enabledReadCount() == 0);

    const int bat0SupportedBeforePercentage = bat0.supportedReadCount();
    const int bat0PercentageBeforePercentage = bat0.percentageReadCount();
    const int bat1SupportedBeforePercentage = bat1.supportedReadCount();
    const int bat1PercentageBeforePercentage = bat1.percentageReadCount();
    bat0.setPercentage(57.0);
    bat0.emitChanged("Percentage");
    drainUntil(bus, [&]() {
      const auto* current = service.deviceForSelector(bat0.path);
      return current != nullptr && current->state.percentage == 57.0;
    });
    assert(bat0.percentageReadCount() > bat0PercentageBeforePercentage);
    assert(bat0.supportedReadCount() == bat0SupportedBeforePercentage);
    assert(bat1.percentageReadCount() == bat1PercentageBeforePercentage);
    assert(bat1.supportedReadCount() == bat1SupportedBeforePercentage);

    const int bat0SupportedBeforeThreshold = bat0.supportedReadCount();
    const int bat1SupportedBeforeThreshold = bat1.supportedReadCount();
    const int bat1PercentageBeforeThreshold = bat1.percentageReadCount();
    bat0.emitChanged("ChargeThresholdEnabled");
    drainUntil(bus, [&]() { return bat0.supportedReadCount() > bat0SupportedBeforeThreshold; });
    assert(bat1.supportedReadCount() == bat1SupportedBeforeThreshold);
    assert(bat1.percentageReadCount() == bat1PercentageBeforeThreshold);
    deviceChanges = 0;
    chargeLimitChanges = 0;

    const auto* initial = findBattery(service, bat0.path);
    assert(initial != nullptr);
    assert(initial->chargeLimit.supported);
    assert(initial->chargeLimit.methodAvailable);
    assert(initial->chargeLimit.enabledAvailable);
    assert(service.enableChargeThreshold(bat0.path, true));
    drainUntil(bus, [&]() { return bat0.pendingCount() == 1; });
    const auto* info = findBattery(service, bat0.path);
    assert(info != nullptr && info->chargeLimit.requestPending);
    assert(info->chargeLimit.requestedEnabled == true);
    assert(chargeLimitChanges >= 1 && deviceChanges == 0);
    PowerTabTestAccess::rebuild(tab);
    assert(PowerTabTestAccess::toggleChecked(tab, 0));
    assert(!PowerTabTestAccess::toggleEnabled(tab, 0));
    assert(PowerTabTestAccess::behavior(tab, 0) == "Starts below 75% · Stops at 80%");
    assert(!PowerTabTestAccess::configuredVisible(tab, 0));
    assert(!PowerTabTestAccess::managementVisible(tab, 0));
    assert(PowerTabTestAccess::controlVisible(tab, 0));

    const int changesBeforeSuccess = chargeLimitChanges;
    bat0.completeSuccess(true);
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat0.path);
      return current != nullptr && !current->chargeLimit.requestPending && current->chargeLimit.enabled;
    });
    info = findBattery(service, bat0.path);
    assert(info != nullptr && !info->chargeLimit.requestedEnabled.has_value());
    assert(info->chargeLimit.operationError == ChargeLimitOperationError::None);
    assert(deviceChanges == 0);
    assert(chargeLimitChanges == changesBeforeSuccess + 1);
    PowerTabTestAccess::rebuild(tab);
    assert(PowerTabTestAccess::toggleChecked(tab, 0));
    assert(PowerTabTestAccess::toggleEnabled(tab, 0));
    assert(PowerTabTestAccess::behavior(tab, 0) == "Starts below 75% · Stops at 80%");
    assert(PowerTabTestAccess::behaviorOpacity(tab, 0) == 1.0F);

    assert(service.enableChargeThreshold(bat0.path, false));
    drainUntil(bus, [&]() { return bat0.pendingCount() == 1; });
    PowerTabTestAccess::rebuild(tab);
    assert(PowerTabTestAccess::behavior(tab, 0) == "Can charge to 100%");
    assert(PowerTabTestAccess::behaviorOpacity(tab, 0) == 1.0F);
    assert(!PowerTabTestAccess::configuredVisible(tab, 0));
    const int enabledReadsBeforeFailure = bat0.enabledReadCount();
    bat0.completeFailure("hardware rejected the write");
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat0.path);
      return current != nullptr && !current->chargeLimit.requestPending;
    });
    info = findBattery(service, bat0.path);
    assert(info != nullptr && info->chargeLimit.enabled);
    assert(info->chargeLimit.operationError == ChargeLimitOperationError::Failed);
    assert(bat0.enabledReadCount() > enabledReadsBeforeFailure);
    PowerTabTestAccess::rebuild(tab);
    assert(PowerTabTestAccess::toggleChecked(tab, 0));
    assert(PowerTabTestAccess::toggleEnabled(tab, 0));
    assert(PowerTabTestAccess::errorVisible(tab, 0));
    assert(PowerTabTestAccess::error(tab, 0) == "Charge thresholds could not be changed.");

    assert(service.enableChargeThreshold(bat0.path, false));
    drainUntil(bus, [&]() { return bat0.pendingCount() == 1; });
    bat0.completeFailure("Operation is not allowed.");
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat0.path);
      return current != nullptr && !current->chargeLimit.requestPending;
    });
    info = findBattery(service, bat0.path);
    assert(info != nullptr && info->chargeLimit.operationError == ChargeLimitOperationError::PermissionDenied);
    PowerTabTestAccess::rebuild(tab);
    assert(PowerTabTestAccess::error(tab, 0) == "Authorization was denied. Charge thresholds were not changed.");

    assert(service.enableChargeThreshold(bat1.path, true));
    drainUntil(bus, [&]() { return bat1.pendingCount() == 1; });
    fake.removeBattery(bat1);
    drainUntil(bus, [&]() { return findBattery(service, bat1.path) == nullptr; });
    PowerTabTestAccess::rebuild(tab);
    assert(PowerTabTestAccess::rowCount(tab) == 1);
    assert(!PowerTabTestAccess::nameVisible(tab, 0));
    fake.readdBattery(bat1);
    drainUntil(bus, [&]() { return findBattery(service, bat1.path) != nullptr; });
    assert(service.enableChargeThreshold(bat1.path, true));
    drainUntil(bus, [&]() { return bat1.pendingCount() == 2; });
    const int changesBeforeStaleReply = chargeLimitChanges;
    bat1.completeFailure("stale request failed");
    for (int i = 0; i < 50; ++i) {
      bus.processPendingEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    info = findBattery(service, bat1.path);
    assert(info != nullptr && info->chargeLimit.requestPending);
    assert(info->chargeLimit.operationError == ChargeLimitOperationError::None);
    assert(chargeLimitChanges == changesBeforeStaleReply);
    bat1.completeSuccess(true);
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat1.path);
      return current != nullptr && !current->chargeLimit.requestPending && current->chargeLimit.enabled;
    });
  }

  // A retained async proxy must not dispatch its signal or reply callback into a destroyed service.
  fake.removeBattery(bat0);
  auto& bat2 = fake.addBattery("BAT2", "Lifetime Battery");
  {
    UPowerService service(bus);
    assert(service.enableChargeThreshold(bat2.path, true));
    drainUntil(bus, [&]() { return bat2.pendingCount() == 1; });
  }
  bat2.emitChanged();
  bat2.completeSuccess(true);
  for (int i = 0; i < 20; ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return 0;
}
