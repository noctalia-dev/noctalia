#include "auth/fingerprint_authenticator.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <chrono>
#include <map>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <utility>

namespace {
  constexpr Logger kLog("fingerprint");

  const sdbus::ServiceName kFprintBusName{"net.reactivated.Fprint"};
  const sdbus::ObjectPath kManagerPath{"/net/reactivated/Fprint/Manager"};
  constexpr auto kManagerInterface = "net.reactivated.Fprint.Manager";
  constexpr auto kDeviceInterface = "net.reactivated.Fprint.Device";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  const sdbus::ServiceName kLoginBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLoginPath{"/org/freedesktop/login1"};
  constexpr auto kLoginManagerInterface = "org.freedesktop.login1.Manager";

  constexpr int kMaxRetries = 3;
  constexpr auto kRetryDelay = std::chrono::milliseconds(250);

  enum class MatchResult {
    Invalid,
    NoMatch,
    Matched,
    Retry,
    SwipeTooShort,
    FingerNotCentered,
    RemoveAndRetry,
    Disconnected,
    UnknownError,
  };

  MatchResult parseVerifyResult(const std::string& result) {
    static const std::map<std::string, MatchResult> kResults = {
        {"verify-no-match", MatchResult::NoMatch},
        {"verify-match", MatchResult::Matched},
        {"verify-retry-scan", MatchResult::Retry},
        {"verify-swipe-too-short", MatchResult::SwipeTooShort},
        {"verify-finger-not-centered", MatchResult::FingerNotCentered},
        {"verify-remove-and-retry", MatchResult::RemoveAndRetry},
        {"verify-disconnected", MatchResult::Disconnected},
        {"verify-unknown-error", MatchResult::UnknownError},
    };
    const auto it = kResults.find(result);
    return it == kResults.end() ? MatchResult::Invalid : it->second;
  }

  bool isRecoverableVerifyStartError(const sdbus::Error& error) {
    // Suspend/resume can drop the claim while the proxy survives; only that case is worth
    // recreating+reclaiming. Other VerifyStart failures are permanent for this lock attempt.
    return error.getName() == sdbus::Error::Name{"net.reactivated.Fprint.Error.ClaimDevice"};
  }
} // namespace

FingerprintAuthenticator::FingerprintAuthenticator(SystemBus& bus) : m_bus(bus) {
  try {
    m_loginManager = sdbus::createProxy(m_bus.connection(), kLoginBusName, kLoginPath);
    // on sleep, stop verification; on resume, restart if active and not aborted
    m_loginManager->uponSignal("PrepareForSleep").onInterface(kLoginManagerInterface).call([this](bool sleeping) {
      m_sleeping = sleeping;
      if (sleeping) {
        stopVerify();
      } else if (m_active && !m_abort) {
        startVerify(false);
      }
    });
  } catch (const sdbus::Error& e) {
    kLog.debug("could not monitor PrepareForSleep: {}", e.what());
    m_loginManager.reset();
  }
}

FingerprintAuthenticator::~FingerprintAuthenticator() { stop(); }

void FingerprintAuthenticator::setAuthenticatedCallback(AuthenticatedCallback callback) {
  m_onAuthenticated = std::move(callback);
}

void FingerprintAuthenticator::setStatusCallback(StatusCallback callback) { m_onStatus = std::move(callback); }

void FingerprintAuthenticator::start() {
  if (m_active) {
    return;
  }

  m_active = true;
  m_abort = false;
  m_retries = 0;
  m_reclaimAttempted = false;
  if (m_sleeping) {
    return;
  }
  startVerify(false);
}

void FingerprintAuthenticator::stop() {
  m_retryTimer.stop();
  m_active = false;
  m_verifying = false;
  m_claiming = false;
  if (!m_abort) {
    releaseDevice();
  } else {
    m_device.reset();
  }
}

bool FingerprintAuthenticator::createDeviceProxy() {
  sdbus::ObjectPath path;
  try {
    auto manager = sdbus::createProxy(m_bus.connection(), kFprintBusName, kManagerPath);
    manager->callMethod("GetDefaultDevice").onInterface(kManagerInterface).storeResultsTo(path);
  } catch (const sdbus::Error& e) {
    kLog.info("no fprintd device available: {}", e.what());
    return false;
  }

  kLog.info("using fingerprint device {}", path.c_str());
  try {
    m_device = sdbus::createProxy(m_bus.connection(), kFprintBusName, path);
  } catch (const sdbus::Error& e) {
    kLog.warn("could not create device proxy: {}", e.what());
    m_device.reset();
    return false;
  }

  m_device->uponSignal("VerifyStatus").onInterface(kDeviceInterface).call([this](const std::string& result, bool done) {
    handleVerifyStatus(result, done);
  });

  m_device->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& /*invalidated*/
            ) {
        if (interfaceName != kDeviceInterface) {
          return;
        }
        const auto it = changed.find("finger-present");
        if (it == changed.end()) {
          return;
        }
        try {
          if (it->second.get<bool>()) {
            emitStatus(i18n::tr("auth.fingerprint.present"), false);
          }
        } catch (const sdbus::Error&) {
        }
      });

  return true;
}

void FingerprintAuthenticator::claimDevice() {
  if (m_device == nullptr || m_claiming) {
    return;
  }
  m_claiming = true;

  m_device->callMethodAsync("Claim")
      .onInterface(kDeviceInterface)
      .withArguments(std::string{})
      .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
        m_claiming = false;
        if (e.has_value()) {
          kLog.info("could not claim fingerprint device: {}", e->what());
          m_verifying = false;
          return;
        }
        kLog.info("claimed fingerprint device");
        if (m_active && !m_sleeping) {
          startVerify(false);
        }
      });
}

void FingerprintAuthenticator::startVerify(bool isRetry) {
  if (!m_active || m_sleeping || m_abort) {
    return;
  }
  m_verifying = true;

  if (m_device == nullptr) {
    if (!createDeviceProxy()) {
      m_verifying = false;
      return;
    }

    emitStatus(i18n::tr("auth.fingerprint.ready"), false);
    claimDevice();
    return;
  }
  if (m_claiming) {
    return;
  }

  m_device->callMethodAsync("VerifyStart")
      .onInterface(kDeviceInterface)
      .withArguments(std::string{"any"})
      .uponReplyInvoke([this, isRetry](std::optional<sdbus::Error> e) {
        if (e.has_value()) {
          kLog.info("could not start fingerprint verification: {}", e->what());
          m_verifying = false;
          // A claim dropped across suspend/resume makes VerifyStart fail with ClaimDevice; drop the
          // stale proxy and recreate+reclaim once. Destroying the proxy here would use-after-free
          // the async reply handler, so defer the reset until after it returns.
          if (!m_reclaimAttempted && m_active && !m_sleeping && !m_abort && isRecoverableVerifyStartError(*e)) {
            m_reclaimAttempted = true;
            m_retryTimer.start(kRetryDelay, [this]() {
              m_device.reset();
              startVerify(false);
            });
            return;
          }
          emitStatus({}, false); // issue loading: drop the status message
          return;
        }
        kLog.debug("fingerprint verification started");
        m_reclaimAttempted = false;
        if (isRetry) {
          m_retries++;
        }
        emitStatus(i18n::tr("auth.fingerprint.ready"), false);
      });
}

void FingerprintAuthenticator::stopVerify() {
  m_retryTimer.stop();
  m_verifying = false;
  if (m_device == nullptr) {
    return;
  }
  try {
    m_device->callMethod("VerifyStop").onInterface(kDeviceInterface);
    kLog.debug("fingerprint verification stopped");
  } catch (const sdbus::Error& e) {
    kLog.debug("could not stop fingerprint verification: {}", e.what());
  }
}

void FingerprintAuthenticator::releaseDevice() {
  if (m_device == nullptr) {
    return;
  }
  try {
    m_device->callMethod("Release").onInterface(kDeviceInterface);
    kLog.info("released fingerprint device");
  } catch (const sdbus::Error& e) {
    kLog.debug("could not release fingerprint device: {}", e.what());
  }
  m_device.reset();
}

void FingerprintAuthenticator::handleVerifyStatus(const std::string& result, bool done) {
  if (!m_active) {
    return;
  }
  if (m_sleeping) {
    stopVerify();
    return;
  }

  kLog.debug("verify status: {} (done={})", result, done);
  const MatchResult match = parseVerifyResult(result);

  bool authenticated = false;
  bool retryInPlace = false;
  // Set when a branch fully owns the post-verify restart decision (capped rearm or deliberate stop),
  // so the generic done-restart below does not also fire.
  bool ownsRestart = false;

  switch (match) {
  case MatchResult::Invalid:
    kLog.warn("unknown fingerprint verify status: {}", result);
    break;
  case MatchResult::Matched:
    stopVerify();
    authenticated = true;
    break;
  case MatchResult::NoMatch:
    stopVerify();
    ownsRestart = true;
    if (m_retries >= kMaxRetries) {
      emitStatus(i18n::tr("auth.fingerprint.too-many-attempts"), true);
    } else {
      emitStatus(i18n::tr("auth.fingerprint.no-match"), true);
      // rearm after a short delay
      m_retryTimer.start(kRetryDelay, [this]() { startVerify(true); });
    }
    break;
  case MatchResult::Retry:
    retryInPlace = true;
    emitStatus(i18n::tr("auth.fingerprint.retry"), false);
    break;
  case MatchResult::SwipeTooShort:
    retryInPlace = true;
    emitStatus(i18n::tr("auth.fingerprint.swipe-too-short"), false);
    break;
  case MatchResult::FingerNotCentered:
    retryInPlace = true;
    emitStatus(i18n::tr("auth.fingerprint.not-centered"), false);
    break;
  case MatchResult::RemoveAndRetry:
    retryInPlace = true;
    emitStatus(i18n::tr("auth.fingerprint.remove-and-retry"), false);
    break;
  case MatchResult::Disconnected:
    stopVerify();
    m_abort = true;
    emitStatus(i18n::tr("auth.fingerprint.disconnected"), true);
    break;
  case MatchResult::UnknownError:
    stopVerify();
    ownsRestart = true;
    if (m_retries >= kMaxRetries) {
      emitStatus(i18n::tr("auth.fingerprint.too-many-attempts"), true);
    } else {
      emitStatus(i18n::tr("auth.fingerprint.error"), true);
      m_retryTimer.start(kRetryDelay, [this]() { startVerify(true); });
    }
    break;
  }

  if (authenticated) {
    if (m_onAuthenticated) {
      m_onAuthenticated();
    }
    return;
  }

  // In-place retries keep verifying; otherwise restart only when fprintd reports done.
  if (!retryInPlace && !ownsRestart && done && !m_abort && m_active && !m_sleeping) {
    startVerify(true);
  }
}

void FingerprintAuthenticator::emitStatus(const std::string& message, bool isError) {
  if (m_onStatus) {
    m_onStatus(message, isError);
  }
}
