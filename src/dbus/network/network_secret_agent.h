#pragma once

#include <functional>
#include <memory>
#include <string>

class SystemBus;

// NetworkManager secret agent. Registers with org.freedesktop.NetworkManager.AgentManager
// on the system bus and answers GetSecrets requests for new Wi-Fi PSK connections.
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
  struct SecretRequest {
    std::string ssid;
    std::string settingName; // e.g. "802-11-wireless-security"
  };

  using RequestCallback = std::function<void(const SecretRequest&)>;

  explicit NetworkSecretAgent(SystemBus& bus);
  ~NetworkSecretAgent();

  NetworkSecretAgent(const NetworkSecretAgent&) = delete;
  NetworkSecretAgent& operator=(const NetworkSecretAgent&) = delete;

  void setRequestCallback(RequestCallback callback);

  // Reply paths for the pending request. Safe no-ops if nothing is pending.
  void submitSecret(const std::string& psk);
  void cancelSecret();

  [[nodiscard]] bool hasPendingRequest() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
