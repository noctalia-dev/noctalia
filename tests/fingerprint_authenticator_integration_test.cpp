// Regression tests for the fingerprint authorization race reported in #3602:
// logind can emit PrepareForSleep(false) before the user's session becomes active,
// causing fprintd to deny Claim or VerifyStart. Previously, the authenticator did
// not retry those denials, leaving the reader idle even after activation.
//
// These tests run the real FingerprintAuthenticator, SystemBus, and TimerManager
// against fake fprintd/logind services on a private D-Bus. The fake services hold
// asynchronous replies until the test explicitly returns PermissionDenied or
// success. This imposes the wake/denial/recovery ordering deterministically;
// timer dispatch is controlled by the test, but delays use the real clock.
// No actual suspend, session activation, polkit policy, or fingerprint hardware
// is exercised: the tests simulate the authorization results of that race.
//
// Specifically, they verify that:
// - Repeated Claim and VerifyStart denials after wake recover once allowed.
// - A denied Claim is retried before verification, and a retained claim is reused.
// - Permanent denial stops after 20 retries, clears the status, and does not
//   consume match attempts; a later lock activation gets a fresh retry budget.
// - Stop and suspend cancel scheduled retries; a denial delivered afterward does
//   not schedule new work (stop cancels the proxy callback, suspend guards it).
// - AlreadyInUse is not retried, while ClaimDevice still triggers reacquisition.
// - Authorization retries preserve the failed-match count and the pending
//   match-retry increment, so repeated mismatches still reach the match limit.
//
// On the unpatched implementation, the first denied Claim fails the assertion
// that a retry is pending. Meson runs this test through dbus-run-session with
// DBUS_SYSTEM_BUS_ADDRESS redirected to the private session bus.

#include "auth/fingerprint_authenticator.h"
#include "core/timer_manager.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class FingerprintAuthenticatorTestAccess {
public:
  static bool sleeping(const FingerprintAuthenticator& auth) { return auth.m_sleeping; }
  static bool idle(const FingerprintAuthenticator& auth) { return !auth.m_claiming && !auth.m_verifying; }
  static bool retryPending(const FingerprintAuthenticator& auth) { return auth.m_retryTimer.active(); }
  static int matchRetries(const FingerprintAuthenticator& auth) { return auth.m_retries; }
  static int authorizationRetries(const FingerprintAuthenticator& auth) { return auth.m_authorizationRetries; }
};

namespace {
  using Access = FingerprintAuthenticatorTestAccess;
  using namespace std::chrono_literals;

  constexpr auto kDeviceInterface = "net.reactivated.Fprint.Device";
  constexpr auto kLoginInterface = "org.freedesktop.login1.Manager";
  const sdbus::ObjectPath kDevicePath{"/net/reactivated/Fprint/Device/0"};
  const sdbus::Error kPermissionDenied{
      sdbus::Error::Name{"net.reactivated.Fprint.Error.PermissionDenied"}, "Session is inactive"
  };

  enum class Method { Claim, VerifyStart };

  // Only the fake service runs on a worker thread. Replies are held until the test
  // explicitly completes them, so the race is an imposed ordering, not a timing bet.
  class FakeServices {
  public:
    FakeServices()
        : m_connection(sdbus::createSessionBusConnection(sdbus::ServiceName{"net.reactivated.Fprint"})),
          m_manager(sdbus::createObject(*m_connection, sdbus::ObjectPath{"/net/reactivated/Fprint/Manager"})),
          m_device(sdbus::createObject(*m_connection, kDevicePath)),
          m_logind(sdbus::createObject(*m_connection, sdbus::ObjectPath{"/org/freedesktop/login1"})) {
      m_connection->requestName(sdbus::ServiceName{"org.freedesktop.login1"});
      m_manager->addVTable(
                   sdbus::registerMethod("GetDefaultDevice").implementedAs([]() { return kDevicePath; })
      ).forInterface("net.reactivated.Fprint.Manager");
      m_device
          ->addVTable(
              sdbus::registerMethod("Claim").implementedAs([this](sdbus::Result<>&& result, std::string user) {
                assert(user.empty());
                enqueue(Method::Claim, std::move(result));
              }),
              sdbus::registerMethod("VerifyStart").implementedAs([this](sdbus::Result<>&& result, std::string finger) {
                assert(finger == "any");
                enqueue(Method::VerifyStart, std::move(result));
              }),
              sdbus::registerMethod("VerifyStop").implementedAs([]() {}),
              sdbus::registerMethod("Release").implementedAs([this]() {
                std::scoped_lock lock(m_mutex);
                m_claimed = false;
              }),
              sdbus::registerSignal("VerifyStatus").withParameters<std::string, bool>()
          )
          .forInterface(kDeviceInterface);
      m_logind->addVTable(sdbus::registerSignal("PrepareForSleep").withParameters<bool>())
          .forInterface(kLoginInterface);
      m_connection->enterEventLoopAsync();
    }

    ~FakeServices() { m_connection->leaveEventLoop(); }

    void sleep(bool sleeping) {
      m_logind->emitSignal("PrepareForSleep").onInterface(kLoginInterface).withArguments(sleeping);
    }

    void verifyStatus(const std::string& result) {
      m_device->emitSignal("VerifyStatus").onInterface(kDeviceInterface).withArguments(result, true);
    }

    bool pending(Method method) const {
      std::scoped_lock lock(m_mutex);
      return m_pending.has_value() && m_method == method;
    }

    int calls(Method method) const {
      std::scoped_lock lock(m_mutex);
      return method == Method::Claim ? m_claimCalls : m_verifyCalls;
    }

    void reply(Method method, std::optional<sdbus::Error> error = std::nullopt) {
      std::optional<sdbus::Result<>> result;
      {
        std::scoped_lock lock(m_mutex);
        assert(m_pending.has_value() && m_method == method);
        result = std::move(m_pending);
        m_pending.reset();
        if (!error) {
          if (method == Method::Claim) {
            assert(!m_claimed && "must not reclaim a device we still own");
            m_claimed = true;
          } else {
            assert(m_claimed && "must retry Claim before VerifyStart");
          }
        }
      }
      if (error) {
        result->returnError(*error);
      } else {
        result->returnResults();
      }
    }

    void dropClaim() {
      std::scoped_lock lock(m_mutex);
      m_claimed = false;
    }

  private:
    void enqueue(Method method, sdbus::Result<>&& result) {
      std::scoped_lock lock(m_mutex);
      assert(!m_pending.has_value() && "overlapping fingerprint requests");
      m_method = method;
      m_pending.emplace(std::move(result));
      if (method == Method::Claim) {
        ++m_claimCalls;
      } else {
        ++m_verifyCalls;
      }
    }

    std::unique_ptr<sdbus::IConnection> m_connection;
    std::unique_ptr<sdbus::IObject> m_manager;
    std::unique_ptr<sdbus::IObject> m_device;
    std::unique_ptr<sdbus::IObject> m_logind;
    mutable std::mutex m_mutex;
    std::optional<sdbus::Result<>> m_pending;
    Method m_method = Method::Claim;
    bool m_claimed = false;
    int m_claimCalls = 0;
    int m_verifyCalls = 0;
  };

  struct Fixture {
    FakeServices services;
    SystemBus bus;
    FingerprintAuthenticator auth{bus};
    std::vector<std::string> statuses;
    int authenticated = 0;

    Fixture() {
      auth.setStatusCallback([this](const std::string& status, bool) { statuses.push_back(status); });
      auth.setAuthenticatedCallback([this]() { ++authenticated; });
    }

    // Timers run only when requested. In particular, assertions after a denied
    // reply cannot race with the retry firing on a slow CI machine.
    template <typename Predicate> void until(Predicate predicate, bool tickTimers = false) {
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      do {
        bus.processPendingEvents();
        if (tickTimers) {
          TimerManager::instance().tick();
        }
        if (std::invoke(predicate)) {
          return;
        }
        std::this_thread::sleep_for(1ms);
      } while (std::chrono::steady_clock::now() < deadline);
      std::println(stderr, "fingerprint_authenticator_integration_test: timed out waiting for state");
      std::abort();
    }

    void pending(Method method, bool tickTimers = false) {
      until([&]() { return services.pending(method); }, tickTimers);
    }

    void sleep(bool sleeping) {
      services.sleep(sleeping);
      until([&]() { return Access::sleeping(auth) == sleeping; });
    }

    void deny(Method method) {
      services.reply(method, kPermissionDenied);
      until([&]() { return Access::idle(auth); });
      assert(authenticated == 0);
    }

    void verified() {
      const auto before = statuses.size();
      services.reply(Method::VerifyStart);
      until([&]() { return statuses.size() > before; });
      assert(statuses.back() == i18n::tr("auth.fingerprint.ready"));
    }

    void startScanning() {
      auth.start();
      pending(Method::Claim);
      services.reply(Method::Claim);
      pending(Method::VerifyStart);
      verified();
    }

    void startAt(Method method) {
      auth.start();
      pending(Method::Claim);
      if (method == Method::VerifyStart) {
        services.reply(Method::Claim);
        pending(Method::VerifyStart);
      }
    }
  };

  void claimAndVerifyRecoverAfterWake() {
    Fixture f;
    f.sleep(true);
    f.auth.start();
    assert(f.services.calls(Method::Claim) == 0);
    f.sleep(false);
    f.pending(Method::Claim);
    for (int attempt = 0; attempt < 3; ++attempt) {
      f.deny(Method::Claim);
      assert(Access::retryPending(f.auth));
      f.pending(Method::Claim, true);
      assert(f.services.calls(Method::VerifyStart) == 0);
    }
    f.services.reply(Method::Claim);
    f.pending(Method::VerifyStart);
    for (int attempt = 0; attempt < 2; ++attempt) {
      f.deny(Method::VerifyStart);
      f.pending(Method::VerifyStart, true);
    }
    f.verified();
    assert(f.services.calls(Method::Claim) == 4);
    assert(f.services.calls(Method::VerifyStart) == 3);
    assert(Access::matchRetries(f.auth) == 0);
    f.services.verifyStatus("verify-match");
    f.until([&]() { return f.authenticated == 1; });
  }

  void retainedClaimRecoversAfterWake() {
    Fixture f;
    f.startScanning();
    f.sleep(true);
    f.sleep(false);
    f.pending(Method::VerifyStart);
    f.deny(Method::VerifyStart);
    f.pending(Method::VerifyStart, true);
    f.verified();
    assert(f.services.calls(Method::Claim) == 1);
    assert(Access::matchRetries(f.auth) == 0);
  }

  void permanentDenialIsBounded(Method method) {
    Fixture f;
    f.startAt(method);
    int retries = 0;
    while (true) {
      f.deny(method);
      if (!Access::retryPending(f.auth)) {
        break;
      }
      assert(++retries <= 20 && "permanent permission denial must stop retrying");
      f.pending(method, true);
    }
    assert(retries == 20);
    assert(Access::matchRetries(f.auth) == 0);
    assert(f.statuses.back().empty());
    // A later lock attempt gets a fresh authorization budget.
    f.auth.stop();
    f.startScanning();
    assert(Access::authorizationRetries(f.auth) == 0);
  }

  void cancellationStopsRetries(Method method, bool suspend, bool replyAfterCancellation) {
    Fixture f;
    f.startAt(method);
    if (!replyAfterCancellation) {
      f.deny(method);
      assert(Access::retryPending(f.auth));
    }
    if (suspend) {
      f.sleep(true);
    } else {
      f.auth.stop();
    }
    if (replyAfterCancellation) {
      // A reply already in flight must not schedule new work while sleeping or
      // after stop() has destroyed the proxy and cancelled its callbacks.
      const auto before = f.statuses.size();
      f.services.reply(method, kPermissionDenied);
      if (suspend) {
        f.until([&]() { return f.statuses.size() > before; });
      }
    }
    assert(!Access::retryPending(f.auth));
    assert(f.authenticated == 0);
  }

  void otherErrorsDoNotRetry(Method method) {
    Fixture f;
    f.startAt(method);
    f.services.reply(method, sdbus::Error{sdbus::Error::Name{"net.reactivated.Fprint.Error.AlreadyInUse"}, "Busy"});
    f.until([&]() { return Access::idle(f.auth); });
    assert(!Access::retryPending(f.auth));
  }

  void droppedClaimStillRecovers() {
    Fixture f;
    f.startScanning();
    f.sleep(true);
    f.services.dropClaim();
    f.sleep(false);
    f.pending(Method::VerifyStart);
    f.services.reply(
        Method::VerifyStart,
        sdbus::Error{sdbus::Error::Name{"net.reactivated.Fprint.Error.ClaimDevice"}, "Claim was dropped"}
    );
    f.until([&]() { return Access::retryPending(f.auth); });
    f.pending(Method::Claim, true);
    f.services.reply(Method::Claim);
    f.pending(Method::VerifyStart);
    f.verified();
    assert(f.services.calls(Method::Claim) == 2);
  }

  void authorizationRetriesPreserveMatchLimit() {
    Fixture f;
    f.startScanning();
    for (int mismatch = 0; mismatch < 3; ++mismatch) {
      f.services.verifyStatus("verify-no-match");
      f.pending(Method::VerifyStart, true);
      for (int denial = 0; denial < 2; ++denial) {
        f.deny(Method::VerifyStart);
        assert(Access::matchRetries(f.auth) == mismatch);
        f.pending(Method::VerifyStart, true);
      }
      f.verified();
      assert(Access::matchRetries(f.auth) == mismatch + 1);
    }
    f.services.verifyStatus("verify-no-match");
    f.until([&]() { return f.statuses.back() == i18n::tr("auth.fingerprint.too-many-attempts"); });
    assert(!Access::retryPending(f.auth));
    assert(f.authenticated == 0);
  }
} // namespace

int main() {
  // Refuse to run against the real system bus, even if invoked outside Meson.
  const char* systemAddress = std::getenv("DBUS_SYSTEM_BUS_ADDRESS");
  const char* sessionAddress = std::getenv("DBUS_SESSION_BUS_ADDRESS");
  if (systemAddress == nullptr || sessionAddress == nullptr || std::string(systemAddress) != sessionAddress) {
    std::println(stderr, "Run on a private dbus-run-session bus with DBUS_SYSTEM_BUS_ADDRESS redirected to it");
    return 1;
  }

  claimAndVerifyRecoverAfterWake();
  retainedClaimRecoversAfterWake();
  for (auto method : {Method::Claim, Method::VerifyStart}) {
    permanentDenialIsBounded(method);
    otherErrorsDoNotRetry(method);
    for (bool suspend : {false, true}) {
      for (bool replyAfterCancellation : {false, true}) {
        cancellationStopsRetries(method, suspend, replyAfterCancellation);
      }
    }
  }
  droppedClaimStillRecovers();
  authorizationRetriesPreserveMatchLimit();
  std::println("fingerprint_authenticator_integration_test: passed");
}
