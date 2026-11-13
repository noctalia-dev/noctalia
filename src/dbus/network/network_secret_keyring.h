#pragma once

#include <functional>
#include <map>
#include <memory>
#include <sdbus-c++/Types.h>
#include <string>
#include <vector>

// Keyring storage for NetworkManager secrets the agent owns, i.e. secrets whose
// flags are NM_SETTING_SECRET_FLAG_AGENT_OWNED ("store the password for this user
// only"). NetworkManager keeps those in memory only and expects the agent to
// remember them across sessions.
//
// Items use the schema nm-applet, GNOME Shell and the VPN auth dialogs share
// (org.freedesktop.NetworkManager.Connection, keyed by connection-uuid,
// setting-name and setting-key), so secrets saved by any of them work for all.
namespace nm_keyring {

  using SecretsDict = std::map<std::string, std::map<std::string, sdbus::Variant>>;

  struct SecretKey {
    std::string setting;
    std::string key;

    bool operator==(const SecretKey&) const = default;
  };

  struct Secret {
    SecretKey id;
    std::string value;

    bool operator==(const Secret&) const = default;
  };

  struct SecretChanges {
    // Agent-owned secrets that have a value.
    std::vector<Secret> save;
    // Secrets whose flags say the agent must not keep them, so any stored copy
    // has to go: system-owned, or "ask every time".
    std::vector<SecretKey> forget;
  };

  // Works out what to keep for `secrets`, an a{sa{sv}} of secret values such as
  // a GetSecrets reply, reading each secret's flags from `connection`. Secrets
  // without flags are left alone. Only settings known to carry secrets are read,
  // since some ordinary properties also have a "-flags" companion.
  [[nodiscard]] SecretChanges secretChanges(const SecretsDict& connection, const SecretsDict& secrets);

  // Turns stored secrets of one setting back into a GetSecrets reply.
  [[nodiscard]] SecretsDict secretsReply(const std::string& setting, const std::map<std::string, std::string>& values);

  // Serialized keyring access. Operations run in order on a worker thread, since
  // the secret service may block on an unlock prompt; lookup results come back
  // on the main thread, and never after the Keyring is destroyed.
  class Keyring {
  public:
    using LookupCallback = std::function<void(std::map<std::string, std::string> values)>;

    Keyring();
    ~Keyring();

    Keyring(const Keyring&) = delete;
    Keyring& operator=(const Keyring&) = delete;

    // Stored secrets of one setting, keyed by setting-key. Empty when there are
    // none or the keyring is unavailable.
    void lookup(std::string uuid, std::string setting, LookupCallback callback);
    void apply(std::string uuid, std::string connectionId, SecretChanges changes);
    // Removes every stored secret of the connection.
    void eraseAll(std::string uuid);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };

} // namespace nm_keyring
