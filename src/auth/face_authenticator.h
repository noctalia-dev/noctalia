#pragma once

#include "core/timer_manager.h"

#include <functional>
#include <memory>
#include <string>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class FaceAuthenticator {
public:
  using AuthenticatedCallback = std::function<void()>;
  using StatusCallback = std::function<void(const std::string& message, bool isError)>;

  explicit FaceAuthenticator(SystemBus& bus);
  ~FaceAuthenticator();

  FaceAuthenticator(const FaceAuthenticator&) = delete;
  FaceAuthenticator& operator=(const FaceAuthenticator&) = delete;

  void setAuthenticatedCallback(AuthenticatedCallback callback);
  void setStatusCallback(StatusCallback callback);

  void start();
  void stop();
  [[nodiscard]] bool isExhausted() const noexcept;

private:
  bool createProxy();
  void claim();
  void startVerify(bool isRetry);
  void stopVerify();
  void release();
  void handleFaceStatus(const std::string& status);
  void handleVerifyStatus(const std::string& result);
  void emitStatus(const std::string& message, bool isError);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_loginManager;
  std::unique_ptr<sdbus::IProxy> m_proxy;

  AuthenticatedCallback m_onAuthenticated;
  StatusCallback m_onStatus;

  Timer m_retryTimer;
  std::string m_user;

  bool m_active = false;
  bool m_verifying = false;
  bool m_claiming = false;
  bool m_sleeping = false;
  bool m_reclaimAttempted = false;
  bool m_faceDetected = false;
  bool m_exhausted = false;
  int m_retries = 0;
};
