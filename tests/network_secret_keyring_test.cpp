#include "dbus/network/network_secret_keyring.h"
#include "tests/test_check.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {

  using nm_keyring::Secret;
  using nm_keyring::SecretKey;
  using nm_keyring::SecretsDict;
  using StringMap = std::map<std::string, std::string>;

  SecretsDict vpnConnection(const StringMap& data, const StringMap& secrets) {
    SecretsDict connection;
    connection["connection"]["uuid"] = sdbus::Variant{std::string("uuid")};
    connection["vpn"]["service-type"] = sdbus::Variant{std::string("org.freedesktop.NetworkManager.openvpn")};
    connection["vpn"]["data"] = sdbus::Variant{data};
    connection["vpn"]["secrets"] = sdbus::Variant{secrets};
    return connection;
  }

  void savesAgentOwnedVpnSecrets() {
    // The OpenVPN setup this was built for: an agent-owned password and a
    // challenge response that must be asked for every time.
    const auto connection = vpnConnection(
        {{"connection-type", "password-tls"}, {"password-flags", "1"}, {"challenge-response-flags", "2"}}, {}
    );
    SecretsDict reply;
    reply["vpn"]["secrets"] = sdbus::Variant{StringMap{
        {"password", "hunter2"},
        {"x-dynamic-challenge-echo:challenge-response", "123456"},
    }};

    const auto changes = nm_keyring::secretChanges(connection, reply);
    // Only the agent-owned password should be saved.
    TEST_CHECK(changes.save == std::vector<Secret>{{.id = {.setting = "vpn", .key = "password"}, .value = "hunter2"}});
    // An always-ask secret should be forgotten, and the hinted answer never stored.
    TEST_CHECK(changes.forget == std::vector<SecretKey>{{.setting = "vpn", .key = "challenge-response"}});
  }

  void readsFlagsFromConnectionNotReply() {
    // SaveSecrets passes the connection as both arguments; a GetSecrets reply
    // carries no flags of its own.
    const auto connection = vpnConnection({{"password-flags", "1"}, {"cert-pass-flags", "0"}}, {{"password", "pw"}});
    const auto changes = nm_keyring::secretChanges(connection, connection);
    TEST_CHECK(changes.save == std::vector<Secret>{{.id = {.setting = "vpn", .key = "password"}, .value = "pw"}});
    // A system-owned secret should be forgotten.
    TEST_CHECK(changes.forget == std::vector<SecretKey>{{.setting = "vpn", .key = "cert-pass"}});

    const auto withoutValue = nm_keyring::secretChanges(vpnConnection({{"password-flags", "1"}}, {}), connection);
    // Flags must come from the connection argument.
    TEST_CHECK(withoutValue.save.size() == 1);
    const auto missingValue = nm_keyring::secretChanges(connection, vpnConnection({{"password-flags", "1"}}, {}));
    // An agent-owned secret without a value must keep what is stored.
    TEST_CHECK(missingValue.save.empty() && missingValue.forget.size() == 1);
  }

  void handlesWirelessSettings() {
    SecretsDict connection;
    connection["connection"]["uuid"] = sdbus::Variant{std::string("uuid")};
    connection["802-11-wireless-security"]["key-mgmt"] = sdbus::Variant{std::string("wpa-psk")};
    connection["802-11-wireless-security"]["psk-flags"] = sdbus::Variant{std::uint32_t{1}};
    connection["802-1x"]["password-flags"] = sdbus::Variant{std::uint32_t{3}};
    // Not secrets, despite the "-flags" companion.
    connection["ipv4"]["dhcp-hostname"] = sdbus::Variant{std::string("host")};
    connection["ipv4"]["dhcp-hostname-flags"] = sdbus::Variant{std::uint32_t{1}};

    SecretsDict secrets = connection;
    secrets["802-11-wireless-security"]["psk"] = sdbus::Variant{std::string("wifi-pass")};
    secrets["802-1x"]["password"] = sdbus::Variant{std::string("eap-pass")};

    const auto changes = nm_keyring::secretChanges(connection, secrets);
    // Only the agent-owned PSK should be saved.
    TEST_CHECK(
        changes.save
        == std::vector<Secret>{{.id = {.setting = "802-11-wireless-security", .key = "psk"}, .value = "wifi-pass"}}
    );
    // AGENT_OWNED with NOT_SAVED must not be kept.
    TEST_CHECK(changes.forget == std::vector<SecretKey>{{.setting = "802-1x", .key = "password"}});
  }

  void buildsReplies() {
    const auto vpn = nm_keyring::secretsReply("vpn", {{"password", "pw"}});
    // VPN secrets should be an a{ss} under vpn.secrets.
    TEST_CHECK(vpn.at("vpn").at("secrets").get<StringMap>() == StringMap{{"password", "pw"}});
    const auto wifi = nm_keyring::secretsReply("802-11-wireless-security", {{"psk", "pass"}});
    // Other secrets should be plain properties of their setting.
    TEST_CHECK(wifi.at("802-11-wireless-security").at("psk").get<std::string>() == "pass");
  }

} // namespace

int main() {
  savesAgentOwnedVpnSecrets();
  readsFlagsFromConnectionNotReply();
  handlesWirelessSettings();
  buildsReplies();
  return 0;
}
