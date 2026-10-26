#pragma once

#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class FingerprintAuthenticator {
public:
  using AuthenticatedCallback = std::function<void()>;
  using StatusCallback = std::function<void(const std::string& message, bool isError)>;

  explicit FingerprintAuthenticator(SystemBus& bus);
  ~FingerprintAuthenticator();

  FingerprintAuthenticator(const FingerprintAuthenticator&) = delete;
  FingerprintAuthenticator& operator=(const FingerprintAuthenticator&) = delete;

  void setAuthenticatedCallback(AuthenticatedCallback callback);
  void setStatusCallback(StatusCallback callback);

  void start();
  void stop();

private:
  friend struct FingerprintAuthenticatorTestPeer;
  struct Session;
  struct Discovery;
  struct Cleanup;

  void invalidate();
  void discover();
  void retryDiscovery();
  void tryCandidate();
  void startVerify(bool isRetry);
  void stopVerify(std::function<void()> continuation = {});
  void releaseDevice(Session& session);
  void deferWork();
  bool current(const Session& session) const;
  void handleVerifyStatus(const std::string& result, bool done);
  void emitStatus(const std::string& message, bool isError);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_loginManager;
  std::unique_ptr<sdbus::IProxy> m_nameProxy;
  // Retired proxies outlive their replies. A pending/uncertain Release blocks only its
  // (unique service owner, object path), never a different reader or a new daemon.
  std::vector<std::unique_ptr<Session>> m_sessions;
  std::vector<std::unique_ptr<Discovery>> m_discoveries;
  Session* m_current = nullptr;
  std::vector<std::string> m_candidates;
  std::string m_owner;
  std::uint64_t m_generation = 0;

  AuthenticatedCallback m_onAuthenticated;
  StatusCallback m_onStatus;

  Timer m_retryTimer;
  Timer m_workTimer;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  std::shared_ptr<Cleanup> m_cleanup;
  bool m_active = false;
  bool m_sleeping = false;
  bool m_abort = false;
  bool m_discovering = false;
  bool m_selecting = false;
  bool m_waitingForCleanup = false;
  int m_discoveryRounds = 0;
  int m_retries = 0;
};
