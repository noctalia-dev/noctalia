#include "auth/face_authenticator.h"

#include "auth/pam_authenticator.h"
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
#include <vector>

namespace {
  constexpr Logger kLog("face-auth");

  const sdbus::ServiceName kGazeBusName{"com.gundulabs.Gaze"};
  const sdbus::ObjectPath kGazePath{"/com/gundulabs/Gaze"};
  constexpr auto kGazeInterface = "com.gundulabs.Gaze";

  const sdbus::ServiceName kLoginBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLoginPath{"/org/freedesktop/login1"};
  constexpr auto kLoginManagerInterface = "org.freedesktop.login1.Manager";

  constexpr int kMaxRetries = 3;
  constexpr auto kRetryDelay = std::chrono::milliseconds(500);

  struct FaceStatusMessage {
    std::string key;
    bool isError;
  };

  std::optional<FaceStatusMessage> captureStatusMessage(const std::string& status) {
    static const std::map<std::string, FaceStatusMessage> kMessages = {
        {"no-face", {"auth.face.no-face", false}},
        {"too-dark", {"auth.face.too-dark", false}},
        {"too-close", {"auth.face.too-close", false}},
        {"too-far", {"auth.face.too-far", false}},
        {"ready", {"auth.face.ready", false}},
        {"usable", {"auth.face.scanning", false}},
        {"not-centered", {"auth.face.no-face", false}},
        {"clipped", {"auth.face.too-close", false}},
    };
    const auto it = kMessages.find(status);
    if (it == kMessages.end()) {
      return std::nullopt;
    }
    return it->second;
  }
  bool isRecoverableClaimError(const sdbus::Error& error) {
    const auto& name = error.getName();
    return name == sdbus::Error::Name{"com.gundulabs.Gaze.Error.ClaimDevice"}
        || name == sdbus::Error::Name{"com.gundulabs.Gaze.Error.NotClaimed"};
  }
} // namespace

FaceAuthenticator::FaceAuthenticator(SystemBus& bus) : m_bus(bus) {
  m_user = PamAuthenticator::currentUsername();

  try {
    m_loginManager = sdbus::createProxy(m_bus.connection(), kLoginBusName, kLoginPath);
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

FaceAuthenticator::~FaceAuthenticator() { stop(); }

void FaceAuthenticator::setAuthenticatedCallback(AuthenticatedCallback callback) {
  m_onAuthenticated = std::move(callback);
}

void FaceAuthenticator::setStatusCallback(StatusCallback callback) { m_onStatus = std::move(callback); }

void FaceAuthenticator::start() {
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

void FaceAuthenticator::stop() {
  m_retryTimer.stop();
  m_active = false;
  m_verifying = false;
  m_claiming = false;
  if (!m_abort) {
    release();
  } else {
    m_proxy.reset();
  }
}

bool FaceAuthenticator::createProxy() {
  try {
    m_proxy = sdbus::createProxy(m_bus.connection(), kGazeBusName, kGazePath);
  } catch (const sdbus::Error& e) {
    kLog.info("gaze daemon not available: {}", e.what());
    m_proxy.reset();
    return false;
  }

  bool hasEnrolledFaces = false;
  try {
    m_proxy->callMethod("HasEnrolledFaces").onInterface(kGazeInterface).withArguments(m_user).storeResultsTo(hasEnrolledFaces);
  } catch (const sdbus::Error& e) {
    kLog.info("could not check enrolled faces: {}", e.what());
    m_proxy.reset();
    return false;
  }
  if (!hasEnrolledFaces) {
    kLog.debug("no enrolled faces for user {}", m_user);
    m_proxy.reset();
    return false;
  }

  bool cameraAvailable = false;
  try {
    m_proxy->callMethod("IsCameraAvailable").onInterface(kGazeInterface).storeResultsTo(cameraAvailable);
  } catch (const sdbus::Error& e) {
    kLog.info("could not check camera availability: {}", e.what());
    m_proxy.reset();
    return false;
  }
  if (!cameraAvailable) {
    kLog.debug("no camera available for face authentication");
    m_proxy.reset();
    return false;
  }

  m_proxy->uponSignal("FaceStatus").onInterface(kGazeInterface).call([this](const std::string& status) {
    handleFaceStatus(status);
  });

  // VerifyStatus carries: result, faces vector, rgb_status, ir_status.
  // We only need the result string.
  m_proxy->uponSignal("VerifyStatus")
      .onInterface(kGazeInterface)
      .call([this](
                const std::string& result,
                const std::vector<sdbus::Struct<std::string, double, double, bool, double, double, bool>>& /*faces*/,
                const std::string& /*rgbStatus*/, const std::string& /*irStatus*/
            ) { handleVerifyStatus(result); });

  return true;
}

void FaceAuthenticator::claim() {
  if (m_proxy == nullptr || m_claiming) {
    return;
  }
  m_claiming = true;

  m_proxy->callMethodAsync("Claim")
      .onInterface(kGazeInterface)
      .withArguments(m_user)
      .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
        m_claiming = false;
        if (e.has_value()) {
          kLog.info("could not claim gaze session: {}", e->what());
          m_verifying = false;
          return;
        }
        kLog.info("claimed gaze session");
        if (m_active && !m_sleeping) {
          startVerify(false);
        }
      });
}

void FaceAuthenticator::startVerify(bool isRetry) {
  if (!m_active || m_sleeping || m_abort) {
    return;
  }
  m_verifying = true;

  if (m_proxy == nullptr) {
    if (!createProxy()) {
      m_verifying = false;
      return;
    }

    emitStatus(i18n::tr("auth.face.ready"), false);
    claim();
    return;
  }
  if (m_claiming) {
    return;
  }

  m_proxy->callMethodAsync("VerifyStart")
      .onInterface(kGazeInterface)
      .withArguments(std::string{"any"})
      .uponReplyInvoke([this, isRetry](std::optional<sdbus::Error> e) {
        if (e.has_value()) {
          kLog.info("could not start face verification: {}", e->what());
          m_verifying = false;
          // A claim dropped across suspend/resume makes VerifyStart fail; drop the
          // stale proxy and recreate+reclaim once. Destroying the proxy here would
          // use-after-free the async reply handler, so defer the reset.
          if (!m_reclaimAttempted && m_active && !m_sleeping && !m_abort && isRecoverableClaimError(*e)) {
            m_reclaimAttempted = true;
            m_retryTimer.start(kRetryDelay, [this]() {
              m_proxy.reset();
              startVerify(false);
            });
            return;
          }
          emitStatus({}, false);
          return;
        }
        kLog.debug("face verification started");
        m_reclaimAttempted = false;
        if (isRetry) {
          m_retries++;
        }
        emitStatus(i18n::tr("auth.face.ready"), false);
      });
}

void FaceAuthenticator::stopVerify() {
  m_retryTimer.stop();
  m_verifying = false;
  if (m_proxy == nullptr) {
    return;
  }
  try {
    m_proxy->callMethod("VerifyStop").onInterface(kGazeInterface);
    kLog.debug("face verification stopped");
  } catch (const sdbus::Error& e) {
    kLog.debug("could not stop face verification: {}", e.what());
  }
}

void FaceAuthenticator::release() {
  if (m_proxy == nullptr) {
    return;
  }
  try {
    m_proxy->callMethod("Release").onInterface(kGazeInterface);
    kLog.info("released gaze session");
  } catch (const sdbus::Error& e) {
    kLog.debug("could not release gaze session: {}", e.what());
  }
  m_proxy.reset();
}

void FaceAuthenticator::handleFaceStatus(const std::string& status) {
  if (!m_active) {
    return;
  }
  const auto msg = captureStatusMessage(status);
  if (msg.has_value()) {
    emitStatus(i18n::tr(msg->key), msg->isError);
  }
}

void FaceAuthenticator::handleVerifyStatus(const std::string& result) {
  if (!m_active) {
    return;
  }
  if (m_sleeping) {
    stopVerify();
    return;
  }

  kLog.debug("face verify result: {}", result);

  if (result == "verify-match") {
    stopVerify();
    if (m_onAuthenticated) {
      m_onAuthenticated();
    }
    return;
  }

  if (result == "verify-no-match") {
    stopVerify();
    if (m_retries >= kMaxRetries) {
      emitStatus(i18n::tr("auth.face.too-many-attempts"), true);
    } else {
      emitStatus(i18n::tr("auth.face.no-match"), true);
      m_retryTimer.start(kRetryDelay, [this]() { startVerify(true); });
    }
    return;
  }

  kLog.warn("unknown face verify result: {}", result);
}

void FaceAuthenticator::emitStatus(const std::string& message, bool isError) {
  if (m_onStatus) {
    m_onStatus(message, isError);
  }
}
