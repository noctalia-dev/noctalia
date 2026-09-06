#pragma once

#include <functional>
#include <memory>
#include <string>

class SystemBus;

// NetworkManager secret agent. Registers with org.freedesktop.NetworkManager.AgentManager
// on the system bus and answers GetSecrets requests for Wi-Fi PSKs and SIM PINs.
//
// The agent is single-slot: one in-flight prompt at a time. Additional GetSecrets
// calls while a prompt is open are rejected with NoSecrets, letting NM fall back
// to its own credential store.
//
// Lifecycle:
//   1. NM calls GetSecrets -> onRequest(SecretRequest) fires on the main thread
//   2. UI prompts user, calls submitSecret() or cancelSecret()
//   3. Deferred sdbus::Result replies to NM
class NetworkSecretAgent {
public:
  enum class SecretKind {
    WifiPsk,
    SimPin,
  };

  struct SecretRequest {
    SecretKind kind = SecretKind::WifiPsk;
    std::string connectionName;
    std::string connectionPath;
  };

  using RequestCallback = std::function<void(const SecretRequest&)>;

  explicit NetworkSecretAgent(SystemBus& bus);
  ~NetworkSecretAgent();

  NetworkSecretAgent(const NetworkSecretAgent&) = delete;
  NetworkSecretAgent& operator=(const NetworkSecretAgent&) = delete;

  void setRequestCallback(RequestCallback callback);

  // Reply paths for the pending request. Safe no-ops if nothing is pending.
  void submitSecret(const std::string& secret);
  void cancelSecret();

  [[nodiscard]] bool hasPendingRequest() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
