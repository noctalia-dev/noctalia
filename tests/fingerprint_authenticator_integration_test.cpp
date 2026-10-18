#include "auth/fingerprint_authenticator.h"
#include "core/timer_manager.h"
#include "dbus/system_bus.h"
#include "tests/test_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Read-only lifetime observation; all state transitions use real fake-bus signals.
struct FingerprintAuthenticatorTestPeer {
  static std::size_t sessions(const FingerprintAuthenticator& auth) { return auth.m_sessions.size(); }
  static std::weak_ptr<void> cleanup(const FingerprintAuthenticator& auth) { return auth.m_cleanup; }
};

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
      manager
          ->addVTable(
              sdbus::registerMethod("GetDefaultDevice").implementedAs([]() { return kDevicePath; }),
              sdbus::registerMethod("GetDevices").implementedAs([]() { return std::vector{kDevicePath}; })
          )
          .forInterface(kManagerInterface);
      device = sdbus::createObject(*connection, kDevicePath);
      device
          ->addVTable(
              sdbus::registerMethod("Claim")
                  .withInputParamNames("username")
                  .implementedAs([this](const std::string& /*username*/) { claim(currentSender()); }),
              sdbus::registerMethod("Release").implementedAs([this](sdbus::Result<> result) {
                cleanup(std::move(result), "Release", true);
              }),
              sdbus::registerMethod("VerifyStart")
                  .withInputParamNames("finger_name")
                  .implementedAs([this](const std::string& /*fingerName*/) { requireOwner("VerifyStart", false); }),
              sdbus::registerMethod("VerifyStop").implementedAs([this](sdbus::Result<> result) {
                cleanup(std::move(result), "VerifyStop", false);
              }),
              sdbus::registerSignal("VerifyStatus").withParameters<std::string, bool>("result", "done")
          )
          .forInterface(kDeviceInterface);
      connection->enterEventLoopAsync();
    }

    ~FakeFprintd() {
      connection->leaveEventLoop();
      cleanupWorkers.clear();
    }

    FakeFprintd(const FakeFprintd&) = delete;
    FakeFprintd& operator=(const FakeFprintd&) = delete;

    void delayCleanup(bool delay) { delayedCleanup = delay; }

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

    void cleanup(sdbus::Result<> result, const std::string& method, bool release) {
      // Retain ownership until close finishes, leaving the event loop free to reject concurrent
      // Claim requests with AlreadyInUse, just like fprintd's asynchronous device close.
      requireOwner(method, false);
      auto finish = [this, result = std::move(result), release]() {
        if (release) {
          std::scoped_lock lock(mutex);
          claimOwner.clear();
        }
        result.returnResults();
      };
      if (delayedCleanup) {
        cleanupWorkers.emplace_back([finish = std::move(finish)]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          finish();
        });
      } else {
        finish();
      }
    }

    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> manager;
    std::unique_ptr<sdbus::IObject> device;
    std::vector<std::jthread> cleanupWorkers;
    mutable std::mutex mutex;
    std::string claimOwner;
    std::vector<Call> log;
    std::atomic<bool> delayedCleanup = false;
  };

  // A re-enumerated reader can coexist with stale, still-exported objects. The manager's
  // default is deliberately not the usable reader; paths and list order carry no freshness.
  class ReenumeratingFprintd {
  public:
    struct Device {
      std::unique_ptr<sdbus::IObject> object;
      std::string owner;
      bool foreign = false;
      bool stallRelease = false;
      int starts = 0;
      int claims = 0;
      int releases = 0;
    };

    ReenumeratingFprintd()
        : connection(sdbus::createSessionBusConnection(kFprintBusName)),
          login(sdbus::createSessionBusConnection(sdbus::ServiceName{"org.freedesktop.login1"})),
          manager(sdbus::createObject(*connection, kManagerPath)),
          loginObject(sdbus::createObject(*login, sdbus::ObjectPath{"/org/freedesktop/login1"})) {
      manager
          ->addVTable(
              sdbus::registerMethod("GetDevices")
                  .implementedAs([this](sdbus::Result<std::vector<sdbus::ObjectPath>> result) {
                    const int delay = discoveryDelay;
                    std::vector<sdbus::ObjectPath> snapshot;
                    {
                      std::scoped_lock lock(mutex);
                      snapshot = listed;
                    }
                    workers.emplace_back([delay, snapshot = std::move(snapshot), result = std::move(result)]() mutable {
                      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                      result.returnResults(snapshot);
                    });
                  }),
              sdbus::registerMethod("GetDefaultDevice").implementedAs([this](sdbus::Result<sdbus::ObjectPath> result) {
                sdbus::ObjectPath snapshot;
                {
                  std::scoped_lock lock(mutex);
                  snapshot = preferred;
                }
                const int delay = discoveryDelay;
                workers.emplace_back([delay, snapshot, result = std::move(result)]() mutable {
                  std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                  result.returnResults(snapshot);
                });
              })
          )
          .forInterface(kManagerInterface);
      loginObject->addVTable(sdbus::registerSignal("PrepareForSleep").withParameters<bool>())
          .forInterface("org.freedesktop.login1.Manager");
      for (int i = 0; i < 3; ++i) {
        auto& d = devices[i];
        d.object = sdbus::createObject(*connection, path(i));
        d.object
            ->addVTable(
                sdbus::registerMethod("Claim").implementedAs([this, i](sdbus::Result<> result, const std::string&) {
                  const auto sender = devices[i].object->getCurrentlyProcessedMessage().getSender();
                  ++claimRequests;
                  const int delay = claimDelay;
                  workers.emplace_back([this, i, sender, delay, result = std::move(result)]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    std::scoped_lock lock(mutex);
                    auto& d = devices[i];
                    ++d.claims;
                    if (d.foreign || !d.owner.empty()) {
                      result.returnError(
                          sdbus::Error(sdbus::Error::Name{"net.reactivated.Fprint.Error.AlreadyInUse"}, "busy")
                      );
                    } else {
                      d.owner = sender;
                      result.returnResults();
                    }
                  });
                }),
                sdbus::registerMethod("VerifyStart")
                    .implementedAs([this, i](sdbus::Result<> result, const std::string&) {
                      const auto sender = devices[i].object->getCurrentlyProcessedMessage().getSender();
                      const int delay = startDelay;
                      workers.emplace_back([this, i, sender, delay, result = std::move(result)]() mutable {
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                        std::scoped_lock lock(mutex);
                        TEST_CHECK(devices[i].owner == sender);
                        ++devices[i].starts;
                        result.returnResults();
                      });
                    }),
                sdbus::registerMethod("VerifyStop").implementedAs([]() {}),
                sdbus::registerMethod("Release").implementedAs([this, i](sdbus::Result<> result) {
                  std::scoped_lock lock(mutex);
                  auto& d = devices[i];
                  ++d.releases;
                  TEST_CHECK(!d.foreign);
                  TEST_CHECK(d.owner == d.object->getCurrentlyProcessedMessage().getSender());
                  if (releaseError) {
                    result.returnError(
                        sdbus::Error(sdbus::Error::Name{"net.reactivated.Fprint.Error.Internal"}, "uncertain")
                    );
                  } else if (d.stallRelease) {
                    stalled.push_back(std::move(result));
                  } else {
                    d.owner.clear();
                    result.returnResults();
                  }
                }),
                sdbus::registerSignal("VerifyStatus").withParameters<std::string, bool>()
            )
            .forInterface(kDeviceInterface);
      }
      connection->enterEventLoopAsync();
      login->enterEventLoopAsync();
    }

    ~ReenumeratingFprintd() {
      connection->leaveEventLoop();
      login->leaveEventLoop();
      workers.clear();
    }
    static sdbus::ObjectPath path(int i) {
      return sdbus::ObjectPath{"/net/reactivated/Fprint/Device/" + std::to_string(i)};
    }
    void sleep(bool sleeping) {
      loginObject->emitSignal("PrepareForSleep").onInterface("org.freedesktop.login1.Manager").withArguments(sleeping);
    }
    void dropName() { connection->releaseName(kFprintBusName); }
    void takeName() { connection->requestName(kFprintBusName); }
    void match(int i) {
      devices[i]
          .object->emitSignal("VerifyStatus")
          .onInterface(kDeviceInterface)
          .withArguments(std::string{"verify-match"}, true);
    }
    int starts(int i) {
      std::scoped_lock lock(mutex);
      return devices[i].starts;
    }
    int releases(int i) {
      std::scoped_lock lock(mutex);
      return devices[i].releases;
    }
    int claims(int i) {
      std::scoped_lock lock(mutex);
      return devices[i].claims;
    }
    void replacement(bool visible = true) {
      std::scoped_lock lock(mutex);
      preferred = path(1);
      devices[1].foreign = true;
      devices[0].stallRelease = true;
      if (devices[0].owner.empty())
        devices[0].foreign = true;
      listed = visible ? std::vector{path(1), path(0), path(2)} : std::vector<sdbus::ObjectPath>{};
    }
    void showReplacement() {
      std::scoped_lock lock(mutex);
      listed = {path(1), path(0), path(2)};
    }
    std::atomic<int> discoveryDelay = 0;
    std::atomic<int> claimDelay = 0;
    std::atomic<int> claimRequests = 0;
    std::atomic<bool> releaseError = false;
    void dropBusName() { connection->releaseName(kFprintBusName); }
    void restoreBusName() { connection->requestName(kFprintBusName); }
    void dropLoginName() { login->releaseName(sdbus::ServiceName{"org.freedesktop.login1"}); }
    // Counters above are read under the fake's mutex.
    std::atomic<int> startDelay = 0;

  private:
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IConnection> login;
    std::unique_ptr<sdbus::IObject> manager;
    std::unique_ptr<sdbus::IObject> loginObject;
    std::mutex mutex;
    Device devices[3];
    std::vector<sdbus::ObjectPath> listed{path(0)};
    sdbus::ObjectPath preferred{path(0)};
    std::vector<std::jthread> workers;
    std::vector<sdbus::Result<>> stalled;
  };

  void pump(SystemBus& bus) {
    bus.processPendingEvents();
    TimerManager::instance().tick();
  }

  template <typename Predicate>
  void drainUntil(SystemBus& bus, Predicate&& predicate, double* maxDispatchMs = nullptr) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
      const auto before = std::chrono::steady_clock::now();
      pump(bus);
      if (maxDispatchMs != nullptr) {
        const auto elapsed = std::chrono::steady_clock::now() - before;
        *maxDispatchMs = std::max(*maxDispatchMs, std::chrono::duration<double, std::milli>(elapsed).count());
      }
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

  void resumeSelectsHealthyAlternativeWhileOldReleaseStalls() {
    ReenumeratingFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    int authenticated = 0;
    auth.setAuthenticatedCallback([&]() { ++authenticated; });
    auth.start();
    drainUntil(bus, [&]() { return fake.starts(0) == 1; });
    fake.sleep(true);
    settle(bus);
    fake.replacement();
    fake.match(0);
    settle(bus);
    TEST_CHECK(authenticated == 0);
    TEST_CHECK(fake.releases(0) == 0); // never call fprintd on sleep
    fake.sleep(false);
    double maxDispatchMs = 0;
    drainUntil(bus, [&]() { return fake.starts(2) == 1; }, &maxDispatchMs);
    TEST_CHECK(maxDispatchMs < 100.0);
    TEST_CHECK(fake.releases(0) == 1); // stalled cleanup must only block this path
    TEST_CHECK(fake.releases(1) == 0); // failed/foreign Claim never authorizes Release
    fake.match(0);
    settle(bus);
    TEST_CHECK(authenticated == 0);
    fake.match(2);
    drainUntil(bus, [&]() { return authenticated == 1; });
    auth.stop();
    drainUntil(bus, [&]() { return fake.releases(2) == 1; });
  }

  void lateHotplugAndDiscoveryDoNotBlockPassword() {
    ReenumeratingFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    auth.start();
    drainUntil(bus, [&]() { return fake.starts(0) == 1; });
    fake.sleep(true);
    settle(bus);
    fake.replacement(false);
    fake.discoveryDelay = 200;
    fake.sleep(false);
    settle(bus);
    fake.showReplacement();
    double maxDispatchMs = 0;
    drainUntil(bus, [&]() { return fake.starts(2) == 1; }, &maxDispatchMs);
    TEST_CHECK(maxDispatchMs < 100.0);
    auth.stop();
    drainUntil(bus, [&]() { return fake.releases(2) == 1; });
  }

  void delayedClaimIsReleasedOnlyAfterSuccessAndNeverAuthenticates() {
    ReenumeratingFprintd fake;
    SystemBus bus;
    auto auth = std::make_unique<FingerprintAuthenticator>(bus);
    int authenticated = 0;
    auth->setAuthenticatedCallback([&]() { ++authenticated; });
    fake.claimDelay = 200;
    auth->start();
    settle(bus);
    auth->stop();
    TEST_CHECK(fake.releases(0) == 0);
    drainUntil(bus, [&]() { return fake.releases(0) == 1; });
    fake.match(0);
    settle(bus);
    TEST_CHECK(authenticated == 0);
    TEST_CHECK(fake.starts(0) == 0);
    fake.discoveryDelay = 200;
    auth->start();
    auth.reset();
    settle(bus);
    settle(bus);
  }

  void stoppedDelayedVerifyReplyCannotAuthenticate() {
    ReenumeratingFprintd fake;
    SystemBus bus;
    auto auth = std::make_unique<FingerprintAuthenticator>(bus);
    int authenticated = 0;
    auth->setAuthenticatedCallback([&]() { ++authenticated; });
    fake.claimDelay = 250;
    auth->start();
    settle(bus);
    fake.sleep(true);
    settle(bus);
    settle(bus);
    TEST_CHECK(fake.releases(0) == 0);
    fake.match(0);
    settle(bus);
    TEST_CHECK(authenticated == 0);
    fake.sleep(false);
    drainUntil(bus, [&]() { return fake.starts(0) == 1; });
    auth->stop();
    drainUntil(bus, [&]() { return fake.releases(0) == 2; });
  }

  void delayedDiscoveryCannotReplaceNewAttempt() {
    ReenumeratingFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    int authenticated = 0;
    auth.setAuthenticatedCallback([&]() { ++authenticated; });
    fake.discoveryDelay = 300;
    auth.start();
    settle(bus);
    auth.stop();
    fake.replacement();
    fake.discoveryDelay = 0;
    auth.start();
    drainUntil(bus, [&]() { return fake.starts(2) == 1; });
    settle(bus);
    settle(bus);
    TEST_CHECK(fake.starts(0) == 0);
    TEST_CHECK(fake.starts(2) == 1);
    fake.match(0);
    settle(bus);
    TEST_CHECK(authenticated == 0);
    fake.match(2);
    drainUntil(bus, [&]() { return authenticated == 1; });
    auth.stop();
    drainUntil(bus, [&]() { return fake.releases(2) == 1; });
  }

  void delayedStartFromPreviousGenerationCannotUnlock() {
    ReenumeratingFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    int authenticated = 0;
    auth.setAuthenticatedCallback([&]() { ++authenticated; });
    fake.startDelay = 300;
    auth.start();
    drainUntil(bus, [&]() { return fake.claims(0) == 1; });
    settle(bus);
    fake.sleep(true);
    settle(bus);
    settle(bus);
    fake.replacement();
    fake.startDelay = 0;
    fake.sleep(false);
    drainUntil(bus, [&]() { return fake.starts(2) == 1; });
    fake.match(0);
    settle(bus);
    TEST_CHECK(authenticated == 0);
    fake.match(2);
    drainUntil(bus, [&]() { return authenticated == 1; });
    auth.stop();
    drainUntil(bus, [&]() { return fake.releases(2) == 1; });
  }

  void daemonReturnAndLateClaimCannotReviveStoppedAttempt() {
    ReenumeratingFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    int authenticated = 0;
    auth.setAuthenticatedCallback([&]() { ++authenticated; });
    auth.start();
    drainUntil(bus, [&]() { return fake.starts(0) == 1; });
    fake.dropName();
    settle(bus);
    fake.replacement();
    fake.takeName();
    drainUntil(bus, [&]() { return fake.starts(2) == 1; });
    fake.match(0);
    settle(bus);
    TEST_CHECK(authenticated == 0);
    auth.stop();
    drainUntil(bus, [&]() { return fake.releases(2) == 1; });
  }

  void destroyDuringClaimReleasesSuccessfulLateClaim() {
    ReenumeratingFprintd fake;
    SystemBus bus;
    auto auth = std::make_unique<FingerprintAuthenticator>(bus);
    fake.claimDelay = 250;
    auth->start();
    settle(bus);
    auth.reset();
    drainUntil(bus, [&]() { return fake.releases(0) == 1; });
    TEST_CHECK(fake.starts(0) == 0);
  }

  void retryBudgetAndInPlaceHintsArePreserved() {
    FakeFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();
    armAndWaitForVerify(auth, bus, fake, self);
    for (const auto& hint :
         {"verify-retry-scan", "verify-swipe-too-short", "verify-finger-not-centered", "verify-remove-and-retry"}) {
      fake.emitVerifyStatus(hint, false);
      settle(bus);
    }
    TEST_CHECK(fake.calls("VerifyStart", self) == 1);
    for (int retry = 0; retry < 3; ++retry) {
      fake.emitVerifyStatus(retry == 1 ? "verify-unknown-error" : "verify-no-match", true);
      drainUntil(bus, [&]() { return fake.accepted("VerifyStart", self) == retry + 2; });
    }
    fake.emitVerifyStatus("verify-no-match", true);
    settle(bus);
    settle(bus);
    settle(bus);
    TEST_CHECK(fake.calls("VerifyStart", self) == 4);
    auth.stop();
    drainUntil(bus, [&]() { return fake.owner().empty(); });
  }

  void cleanupDoesNotWaitForDaemonReplies() {
    FakeFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();
    armAndWaitForVerify(auth, bus, fake, self);
    settle(bus);

    fake.delayCleanup(true);
    const auto before = std::chrono::steady_clock::now();
    auth.stop(); // Password submission must be able to start PAM immediately.
    TEST_CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(200));
    drainUntil(bus, [&]() { return fake.accepted("Release", self) == 1 && fake.owner().empty(); });
    fake.delayCleanup(false);
    armAndWaitForVerify(auth, bus, fake, self);
    auth.stop();
    drainUntil(bus, [&]() { return fake.accepted("Release", self) == 2; });
  }

  void matchDoesNotWaitForDaemonReplies() {
    FakeFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();
    armAndWaitForVerify(auth, bus, fake, self);
    settle(bus);

    bool authenticated = false;
    auth.setAuthenticatedCallback([&]() {
      authenticated = true;
      auth.stop();
    });
    fake.delayCleanup(true);
    fake.emitVerifyStatus("verify-match", true);
    const auto before = std::chrono::steady_clock::now();
    drainUntil(bus, [&]() { return authenticated; });
    TEST_CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(200));
    drainUntil(bus, [&]() { return fake.accepted("Release", self) == 1; });
  }

  void restartWaitsForReleaseCompletion(bool stopAfterReply) {
    FakeFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();
    int authenticated = 0;
    auth.setAuthenticatedCallback([&]() { ++authenticated; });
    armAndWaitForVerify(auth, bus, fake, self);
    fake.delayCleanup(true);
    auth.stop();
    drainUntil(bus, [&]() { return fake.accepted("Release", self) == 1; });
    TEST_CHECK(fake.owner() == self);

    // Password PAM is a separate sender and can fail before the old close completes.
    auto password = sdbus::createSessionBusConnection();
    auto device = sdbus::createProxy(*password, kFprintBusName, kDevicePath);
    bool busy = false;
    try {
      device->callMethod("Claim").onInterface(kDeviceInterface).withArguments(std::string{});
    } catch (const sdbus::Error& e) {
      busy = e.getName() == "net.reactivated.Fprint.Error.AlreadyInUse";
    }
    TEST_CHECK(busy);

    auth.start();
    if (stopAfterReply) {
      // The reply has queued proxy destruction, but stop() must not cancel that timer either.
      for (int i = 0; i < 400; ++i) {
        bus.processPendingEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      TEST_CHECK(fake.owner().empty());
    }
    // Repeated handoffs must not cancel completion or submit an overlapping Release.
    auth.stop();
    auth.start();
    fake.emitVerifyStatus("verify-match", true); // Late result from the released attempt.
    double maxDispatchMs = 0.0;
    drainUntil(bus, [&]() { return fake.accepted("VerifyStart", self) == 2; }, &maxDispatchMs);
    TEST_CHECK(maxDispatchMs < 100.0);
    TEST_CHECK(fake.calls("Claim", self) == 2);
    TEST_CHECK(fake.accepted("Claim", self) == 2);
    TEST_CHECK(fake.calls("Release", self) == 1);
    TEST_CHECK(fake.owner() == self);
    TEST_CHECK(authenticated == 0);
    fake.delayCleanup(false);
    auth.stop();
    drainUntil(bus, [&]() { return fake.owner().empty(); });
  }

  void destroyDuringRelease(bool afterReply) {
    FakeFprintd fake;
    SystemBus bus;
    auto auth = std::make_unique<FingerprintAuthenticator>(bus);
    const std::string self = bus.connection().getUniqueName();
    armAndWaitForVerify(*auth, bus, fake, self);
    fake.delayCleanup(true);
    auth->stop();
    drainUntil(bus, [&]() { return fake.accepted("Release", self) == 1; });
    if (afterReply) {
      // Dispatch the reply without ticking timers: proxy reset is still queued at destruction.
      for (int i = 0; i < 400; ++i) {
        bus.processPendingEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      TEST_CHECK(fake.owner().empty());
    }
    auth.reset();
    drainUntil(bus, [&]() { return fake.owner().empty(); });
    settle(bus);
    TEST_CHECK(fake.calls("Release", self) == 1);
    TEST_CHECK(fake.calls("Claim", self) == 1);
    TEST_CHECK(fake.calls("VerifyStart", self) == 1);
  }

  void retryWaitsForVerifyStopReply() {
    FakeFprintd fake;
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    const std::string self = bus.connection().getUniqueName();
    armAndWaitForVerify(auth, bus, fake, self);
    settle(bus);
    fake.delayCleanup(true);
    fake.emitVerifyStatus("verify-no-match", true);
    const auto before = std::chrono::steady_clock::now();
    drainUntil(bus, [&]() { return fake.accepted("VerifyStop", self) == 1; });
    // Stop must be nonblocking, but a retry must still wait for the stop reply.
    TEST_CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100));
    TEST_CHECK(fake.accepted("VerifyStart", self) == 1);
    drainUntil(bus, [&]() { return fake.accepted("VerifyStart", self) == 2; });
    TEST_CHECK(fake.calls("Claim", self) == 1);
    fake.delayCleanup(false);
    auth.stop();
    drainUntil(bus, [&]() { return fake.owner().empty(); });
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
    drainUntil(bus, [&]() { return fake.accepted("Release", self) == 1; });
    TEST_CHECK(fake.owner().empty());

    // The next attempt from the same long-lived connection must be able to claim again (#4591).
    armAndWaitForVerify(auth, bus, fake, self);
    TEST_CHECK(fake.calls("Claim", self) == 2);
    TEST_CHECK(fake.accepted("Claim", self) == 2);

    auth.stop();
    drainUntil(bus, [&]() { return fake.accepted("Release", self) == 2; });
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
    drainUntil(bus, [&]() { return fake.accepted("Release", self) == 1; });
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

  void departedOwnersReclaimRetiredSessions() {
    SystemBus bus;
    FingerprintAuthenticator auth(bus);
    for (int cycle = 0; cycle < 4; ++cycle) {
      auto fake = std::make_unique<ReenumeratingFprintd>();
      fake->releaseError = cycle % 2 == 0;
      auth.start();
      drainUntil(bus, [&]() { return fake->starts(0) == 1; });
      if (cycle % 2 != 0)
        fake->replacement(); // hold the Release reply across real owner departure
      auth.stop();
      drainUntil(bus, [&]() { return fake->releases(0) == 1; });
      settle(bus);
      TEST_CHECK(FingerprintAuthenticatorTestPeer::sessions(auth) == 1);
      if (cycle % 2 == 0) {
        // Losing only the well-known name does NOT prove the unique owner is gone.
        fake->dropBusName();
        settle(bus);
        fake->restoreBusName();
        settle(bus);
        auth.start();
        settle(bus);
        TEST_CHECK(fake->claims(0) == 1); // still blocked on the live same-owner/path
        auth.stop();
      }
      // Replace the actual daemon before its delayed old Release callback runs.
      // The still-live old unique owner must not block the new owner's same path.
      fake->dropBusName();
      fake->dropLoginName();
      auto replacement = std::make_unique<ReenumeratingFprintd>();
      auth.start();
      drainUntil(bus, [&]() { return replacement->starts(0) == 1; });
      fake.reset(); // actual connection destruction emits unique NameOwnerChanged
      drainUntil(bus, [&]() { return FingerprintAuthenticatorTestPeer::sessions(auth) == 1; });
      TEST_CHECK(replacement->releases(0) == 0);
      auth.stop();
      drainUntil(bus, [&]() { return replacement->releases(0) == 1; });
      replacement.reset();
      drainUntil(bus, [&]() { return FingerprintAuthenticatorTestPeer::sessions(auth) == 0; });
    }
  }

  void destroyedClaimTracksSleep(bool initiallySleeping) {
    ReenumeratingFprintd fake;
    SystemBus bus;
    auto auth = std::make_unique<FingerprintAuthenticator>(bus);
    fake.claimDelay = 600;
    auth->start();
    drainUntil(bus, [&]() { return fake.claimRequests == 1; });
    if (initiallySleeping) {
      fake.sleep(true);
      settle(bus);
    }
    const auto cleanup = FingerprintAuthenticatorTestPeer::cleanup(*auth);
    auth.reset();
    // Both directions must be observed after the authenticator is gone.
    fake.sleep(!initiallySleeping);
    settle(bus);
    drainUntil(bus, [&]() { return fake.claims(0) == 1; });
    settle(bus);
    if (!initiallySleeping) {
      TEST_CHECK(fake.releases(0) == 0);
      fake.sleep(false);
    }
    drainUntil(bus, [&]() { return fake.releases(0) == 1; });
    settle(bus);
    TEST_CHECK(fake.starts(0) == 0);
    TEST_CHECK(cleanup.expired()); // coordinator/proxies are reclaimed after completion
    // The shared connection must not remain claimed after detached cleanup.
    FingerprintAuthenticator next(bus);
    fake.claimDelay = 0;
    next.start();
    drainUntil(bus, [&]() { return fake.starts(0) == 1; });
    next.stop();
    drainUntil(bus, [&]() { return fake.releases(0) == 2; });
  }

  void destroyedStartTracksSleep() {
    ReenumeratingFprintd fake;
    SystemBus bus;
    auto auth = std::make_unique<FingerprintAuthenticator>(bus);
    fake.startDelay = 600;
    auth->start();
    drainUntil(bus, [&]() { return fake.claims(0) == 1; });
    settle(bus);
    auth.reset();
    fake.sleep(true);
    settle(bus);
    drainUntil(bus, [&]() { return fake.starts(0) == 1; });
    settle(bus);
    TEST_CHECK(fake.releases(0) == 0);
    fake.sleep(false);
    drainUntil(bus, [&]() { return fake.releases(0) == 1; });
    settle(bus);
  }

} // namespace

int main() {
  const std::string selected = std::getenv("FINGERPRINT_REVIEW_CASE") ? std::getenv("FINGERPRINT_REVIEW_CASE") : "";
  if (!selected.empty()) {
    if (selected == "owner")
      departedOwnersReclaimRetiredSessions();
    else if (selected == "destroy-asleep")
      destroyedClaimTracksSleep(true);
    else if (selected == "destroy-awake")
      destroyedClaimTracksSleep(false);
    else if (selected == "destroy-start")
      destroyedStartTracksSleep();
    else
      TEST_CHECK(false);
    return 0;
  }
  departedOwnersReclaimRetiredSessions();
  destroyedClaimTracksSleep(true);
  destroyedClaimTracksSleep(false);
  destroyedStartTracksSleep();
  delayedDiscoveryCannotReplaceNewAttempt();
  delayedStartFromPreviousGenerationCannotUnlock();
  stoppedDelayedVerifyReplyCannotAuthenticate();
  destroyDuringClaimReleasesSuccessfulLateClaim();
  daemonReturnAndLateClaimCannotReviveStoppedAttempt();
  retryBudgetAndInPlaceHintsArePreserved();
  resumeSelectsHealthyAlternativeWhileOldReleaseStalls();
  lateHotplugAndDiscoveryDoNotBlockPassword();
  delayedClaimIsReleasedOnlyAfterSuccessAndNeverAuthenticates();
  restartWaitsForReleaseCompletion(false);
  restartWaitsForReleaseCompletion(true);
  destroyDuringRelease(false);
  destroyDuringRelease(true);
  retryWaitsForVerifyStopReply();
  cleanupDoesNotWaitForDaemonReplies();
  matchDoesNotWaitForDaemonReplies();
  disconnectedVerifyReleasesClaimOnStop();
  matchedVerifyReleasesClaimOnStop();
  foreignClaimIsLeftAlone();
  stopAfterDeviceRemovalIsBounded();
  stopAfterServiceExitIsBounded();
  return 0;
}
