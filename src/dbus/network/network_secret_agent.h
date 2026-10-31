#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class SystemBus;

// NetworkManager secret agent. Registers with org.freedesktop.NetworkManager.AgentManager
// on the system bus and answers GetSecrets requests for new Wi-Fi connections,
// both pre-shared key ("802-11-wireless-security") and 802.1X ("802-1x"), and
// for VPN connections ("vpn").
//
// VPN secrets depend on the plugin, so the plugin's auth dialog helper decides
// what to ask for (see vpn_auth_dialog.h). The helper runs before the prompt is
// shown; in external UI mode its fields arrive in SecretRequest::fields, while a
// legacy helper prompts in its own window and the request is answered without
// involving the UI at all.
//
// The agent is single-slot: one in-flight prompt at a time. Additional GetSecrets
// calls while a prompt is open are rejected with NoSecrets, letting NM fall back
// to its own credential store.
//
// Lifecycle:
//   1. NM calls GetSecrets -> onRequest(SecretRequest) fires on the main thread
//   2. UI prompts user, calls submitSecret()/submitSecrets() or cancelSecret()
//   3. Deferred sdbus::Result replies to NM
class NetworkSecretAgent {
public:
  struct SecretField {
    std::string key;
    std::string label;
    std::string value;
    bool secret = true;
    // Shown as typed even though it is a secret, e.g. a one-time code.
    bool echo = false;
  };

  struct SecretRequest {
    std::string ssid;
    std::string settingName; // e.g. "802-11-wireless-security"
    // VPN only: the connection's name, the plugin's explanation of what it needs,
    // and the fields to ask for, answered through submitSecrets().
    std::string connectionName;
    std::string message;
    std::vector<SecretField> fields;

    [[nodiscard]] bool isVpn() const noexcept { return settingName == "vpn"; }
  };

  using RequestCallback = std::function<void(const SecretRequest&)>;

  explicit NetworkSecretAgent(SystemBus& bus);
  ~NetworkSecretAgent();

  NetworkSecretAgent(const NetworkSecretAgent&) = delete;
  NetworkSecretAgent& operator=(const NetworkSecretAgent&) = delete;

  void setRequestCallback(RequestCallback callback);

  // Reply paths for the pending request. Safe no-ops if nothing is pending. The
  // secret is written under the key the pending setting expects: "psk" for
  // pre-shared keys, "password" for 802.1X.
  void submitSecret(const std::string& secret);
  // VPN reply path: values for the request's fields, keyed by SecretField::key.
  void submitSecrets(const std::map<std::string, std::string>& values);
  void cancelSecret();

  [[nodiscard]] bool hasPendingRequest() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
