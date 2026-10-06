#include "auth/fingerprint_authenticator.h"
#include "core/timer_manager.h"
#include "dbus/system_bus.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

  const sdbus::ServiceName kFprintBusName{"net.reactivated.Fprint"};
  const sdbus::ObjectPath kManagerPath{"/net/reactivated/Fprint/Manager"};
  const sdbus::ObjectPath kDevicePath{"/net/reactivated/Fprint/Device/0"};
  constexpr auto kManagerInterface = "net.reactivated.Fprint.Manager";
  constexpr auto kDeviceInterface = "net.reactivated.Fprint.Device";

  // Mirrors fprintd's claim rules (_fprint_device_check_claimed in fprintd's device.c): one session per
  // device, Claim fails while any session exists, and only the claiming sender may use or release it.
  class FakeFprintd {
  public:
    struct Call {
      std::string method;
      std::string sender;
      bool accepted = false;
    };

    FakeFprintd()
        : connection(sdbus::createSessionBusConnection(kFprintBusName)),
          manager(sdbus::createObject(*connection, kManagerPath)) {
      manager->addVTable(
                 sdbus::registerMethod("GetDefaultDevice").implementedAs([]() { return kDevicePath; })
      ).forInterface(kManagerInterface);
      device = sdbus::createObject(*connection, kDevicePath);
      device
          ->addVTable(
              sdbus::registerMethod("Claim")
                  .withInputParamNames("username")
                  .implementedAs([this](const std::string& /*username*/) { claim(currentSender()); }),
              sdbus::registerMethod("Release").implementedAs([this]() { requireOwner("Release", true); }),
              sdbus::registerMethod("VerifyStart")
                  .withInputParamNames("finger_name")
                  .implementedAs([this](const std::string& /*fingerName*/) { requireOwner("VerifyStart", false); }),
              sdbus::registerMethod("VerifyStop").implementedAs([this]() { requireOwner("VerifyStop", false); }),
              sdbus::registerSignal("VerifyStatus").withParameters<std::string, bool>("result", "done")
          )
          .forInterface(kDeviceInterface);
      connection->enterEventLoopAsync();
    }

    ~FakeFprintd() { connection->leaveEventLoop(); }

    FakeFprintd(const FakeFprintd&) = delete;
    FakeFprintd& operator=(const FakeFprintd&) = delete;

    void emitVerifyStatus(const std::string& result, bool done) {
      device->emitSignal("VerifyStatus").onInterface(kDeviceInterface).withArguments(result, done);
    }

    // fprintd unexports the device object when the reader is unplugged.
    void removeDevice() { device->unregister(); }

    // fprintd exits or crashes: the name has no owner and nothing activates it on the test bus.
    void dropBusName() { connection->releaseName(kFprintBusName); }

    [[nodiscard]] std::string owner() const {
      std::scoped_lock lock(mutex);
      return claimOwner;
    }

    [[nodiscard]] long calls(const std::string& method, const std::string& sender) const {
      std::scoped_lock lock(mutex);
      return std::ranges::count_if(log, [&](const Call& c) { return c.method == method && c.sender == sender; });
    }

    [[nodiscard]] long accepted(const std::string& method, const std::string& sender) const {
      std::scoped_lock lock(mutex);
      return std::ranges::count_if(log, [&](const Call& c) {
        return c.method == method && c.sender == sender && c.accepted;
      });
    }

    [[nodiscard]] std::vector<std::string> acceptedMethods(const std::string& sender) const {
      std::scoped_lock lock(mutex);
      std::vector<std::string> out;
      for (const auto& c : log) {
        if (c.sender == sender && c.accepted) {
          out.push_back(c.method);
        }
      }
      return out;
    }

  private:
    [[nodiscard]] std::string currentSender() const { return device->getCurrentlyProcessedMessage().getSender(); }

    void claim(const std::string& sender) {
      {
        std::scoped_lock lock(mutex);
        const bool free = claimOwner.empty();
        log.push_back({"Claim", sender, free});
        if (free) {
          claimOwner = sender;
          return;
        }
      }
      throw sdbus::Error(sdbus::Error::Name{"net.reactivated.Fprint.Error.AlreadyInUse"}, "Device was already claimed");
    }

    void requireOwner(const std::string& method, bool release) {
      sdbus::Error::Name errorName;
      std::string errorMessage;
      const std::string sender = currentSender();
      {
        std::scoped_lock lock(mutex);
        if (claimOwner.empty()) {
          errorName = sdbus::Error::Name{"net.reactivated.Fprint.Error.ClaimDevice"};
          errorMessage = "Device was not claimed before use";
        } else if (claimOwner != sender) {
          errorName = sdbus::Error::Name{"net.reactivated.Fprint.Error.AlreadyInUse"};
          errorMessage = "Device already in use by another user";
        }
        log.push_back({method, sender, errorName.empty()});
        if (errorName.empty()) {
          if (release) {
            claimOwner.clear();
          }
          return;
        }
      }
      throw sdbus::Error(std::move(errorName), std::move(errorMessage));
    }

    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> manager;
    std::unique_ptr<sdbus::IObject> device;
    mutable std::mutex mutex;
    std::string claimOwner;
    std::vector<Call> log;
  };

  void pump(SystemBus& bus) {
    bus.processPendingEvents();
    TimerManager::instance().tick();
  }

  template <typename Predicate> void drainUntil(SystemBus& bus, Predicate&& predicate) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
      pump(bus);
      if (std::invoke(predicate)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    noctalia::test::failCheck("timed out waiting for D-Bus state");
  }

  // Lets any stray follow-up calls (re-arms, retries) reach the fake before asserting on counts.
  void settle(SystemBus& bus) {
    for (int i = 0; i < 100; ++i) {
      pump(bus);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void armAndWaitForVerify(FingerprintAuthenticator& auth, SystemBus& bus, FakeFprintd& fake, const std::string& self) {
    const long before = fake.accepted("VerifyStart", self);
    auth.start();
    drainUntil(bus, [&]() { return fake.accepted("VerifyStart", self) == before + 1; });
    TEST_CHECK(fake.owner() == self);
  }

  void disconnectedVerifyReleasesClaimOnStop() {
    FakeFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();

    armAndWaitForVerify(auth, bus, fake, self);

    // libfprint reports TOO_HOT (and PROTO/REMOVED) as verify-disconnected; fprintd keeps the claim.
    fake.emitVerifyStatus("verify-disconnected", true);
    drainUntil(bus, [&]() { return fake.calls("VerifyStop", self) == 1; });
    settle(bus);
    TEST_CHECK(fake.calls("VerifyStart", self) == 1); // a disconnect still does not re-arm verification
    TEST_CHECK(fake.owner() == self);                 // the claim is held until cleanup, as before

    auth.stop();
    TEST_CHECK(fake.accepted("Release", self) == 1);
    TEST_CHECK(fake.owner().empty());

    // The next attempt from the same long-lived connection must be able to claim again (#4591).
    armAndWaitForVerify(auth, bus, fake, self);
    TEST_CHECK(fake.calls("Claim", self) == 2);
    TEST_CHECK(fake.accepted("Claim", self) == 2);

    auth.stop();
    TEST_CHECK(fake.owner().empty());
  }

  void matchedVerifyReleasesClaimOnStop() {
    FakeFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();
    int authenticated = 0;
    auth.setAuthenticatedCallback([&authenticated]() { ++authenticated; });

    armAndWaitForVerify(auth, bus, fake, self);
    fake.emitVerifyStatus("verify-match", true);
    drainUntil(bus, [&]() { return authenticated == 1; });

    auth.stop();
    TEST_CHECK(fake.owner().empty());
    const std::vector<std::string> expected{"Claim", "VerifyStart", "VerifyStop", "Release"};
    TEST_CHECK(fake.acceptedMethods(self) == expected);
  }

  void foreignClaimIsLeftAlone() {
    FakeFprintd fake;
    auto foreign = sdbus::createSessionBusConnection();
    auto foreignDevice = sdbus::createProxy(*foreign, kFprintBusName, kDevicePath);
    foreignDevice->callMethod("Claim").onInterface(kDeviceInterface).withArguments(std::string{});
    const std::string foreignName = foreign->getUniqueName();
    TEST_CHECK(fake.owner() == foreignName);

    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();

    auth.start();
    drainUntil(bus, [&]() { return fake.calls("Claim", self) == 1; });
    settle(bus);
    TEST_CHECK(fake.accepted("Claim", self) == 0);
    TEST_CHECK(fake.calls("VerifyStart", self) == 0);

    auth.stop();
    settle(bus);
    TEST_CHECK(fake.owner() == foreignName);
    TEST_CHECK(fake.accepted("Release", self) == 0);
    TEST_CHECK(fake.calls("Claim", self) == 1); // no reclaim loop
  }

  void stopAfterDeviceRemovalIsBounded() {
    FakeFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();

    armAndWaitForVerify(auth, bus, fake, self);
    fake.emitVerifyStatus("verify-disconnected", true);
    drainUntil(bus, [&]() { return fake.calls("VerifyStop", self) == 1; });
    fake.removeDevice();

    const auto begin = std::chrono::steady_clock::now();
    auth.stop();
    TEST_CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
    TEST_CHECK(fake.calls("Release", self) == 0); // the unexported object never sees the call
  }

  void stopAfterServiceExitIsBounded() {
    FakeFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();

    armAndWaitForVerify(auth, bus, fake, self);
    fake.emitVerifyStatus("verify-disconnected", true);
    drainUntil(bus, [&]() { return fake.calls("VerifyStop", self) == 1; });
    fake.dropBusName();

    const auto begin = std::chrono::steady_clock::now();
    auth.stop();
    TEST_CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
  }

} // namespace

int main() {
  disconnectedVerifyReleasesClaimOnStop();
  matchedVerifyReleasesClaimOnStop();
  foreignClaimIsLeftAlone();
  stopAfterDeviceRemovalIsBounded();
  stopAfterServiceExitIsBounded();
  return 0;
}
