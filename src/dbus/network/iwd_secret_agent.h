#pragma once

#include <functional>
#include <memory>
#include <string>

class SystemBus;

// iwd secret agent. Registers with net.connman.iwd.AgentManager
// on the system bus and answers RequestPassphrase requests for new Wi-Fi PSK connections.
//
// The agent is single-slot: one in-flight request at a time. Additional RequestPassphrase
// calls while a request is pending are rejected, letting iwd fall back to its own handling.
//
// Lifecycle:
//   1. iwd calls RequestPassphrase -> onRequest(SecretRequest) fires on the main thread
//   2. UI prompts user, calls submitSecret() or cancelSecret()
//   3. Deferred sdbus::Result replies to iwd
class IwdSecretAgent {
public:
  struct SecretRequest {
    std::string ssid;
  };

  using RequestCallback = std::function<void(const SecretRequest&)>;

  explicit IwdSecretAgent(SystemBus& bus);
  ~IwdSecretAgent();

  IwdSecretAgent(const IwdSecretAgent&) = delete;
  IwdSecretAgent& operator=(const IwdSecretAgent&) = delete;

  void setRequestCallback(RequestCallback callback);

  // Reply paths for the pending request. Safe no-ops if nothing is pending.
  void submitSecret(const std::string& psk);
  void cancelSecret();

  bool hasPendingRequest() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
