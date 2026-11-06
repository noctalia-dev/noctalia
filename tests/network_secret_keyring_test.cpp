#include "dbus/network/network_secret_keyring.h"

#include <cstdint>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

  using nm_keyring::Secret;
  using nm_keyring::SecretKey;
  using nm_keyring::SecretsDict;
  using StringMap = std::map<std::string, std::string>;

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "network_secret_keyring_test: {}", message);
    }
    return condition;
  }

  SecretsDict vpnConnection(const StringMap& data, const StringMap& secrets) {
    SecretsDict connection;
    connection["connection"]["uuid"] = sdbus::Variant{std::string("uuid")};
    connection["vpn"]["service-type"] = sdbus::Variant{std::string("org.freedesktop.NetworkManager.openvpn")};
    connection["vpn"]["data"] = sdbus::Variant{data};
    connection["vpn"]["secrets"] = sdbus::Variant{secrets};
    return connection;
  }

  bool savesAgentOwnedVpnSecrets() {
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
    bool ok = expect(
        changes.save == std::vector<Secret>{{.id = {.setting = "vpn", .key = "password"}, .value = "hunter2"}},
        "only the agent-owned password should be saved"
    );
    ok = expect(
             changes.forget == std::vector<SecretKey>{{.setting = "vpn", .key = "challenge-response"}},
             "an always-ask secret should be forgotten, and the hinted answer never stored"
         )
        && ok;
    return ok;
  }

  bool readsFlagsFromConnectionNotReply() {
    // SaveSecrets passes the connection as both arguments; a GetSecrets reply
    // carries no flags of its own.
    const auto connection = vpnConnection({{"password-flags", "1"}, {"cert-pass-flags", "0"}}, {{"password", "pw"}});
    const auto changes = nm_keyring::secretChanges(connection, connection);
    bool ok = expect(
        changes.save == std::vector<Secret>{{.id = {.setting = "vpn", .key = "password"}, .value = "pw"}},
        "SaveSecrets should store the agent-owned secret it carries"
    );
    ok = expect(
             changes.forget == std::vector<SecretKey>{{.setting = "vpn", .key = "cert-pass"}},
             "a system-owned secret should be forgotten"
         )
        && ok;

    const auto withoutValue = nm_keyring::secretChanges(vpnConnection({{"password-flags", "1"}}, {}), connection);
    ok = expect(withoutValue.save.size() == 1, "flags must come from the connection argument") && ok;
    const auto missingValue = nm_keyring::secretChanges(connection, vpnConnection({{"password-flags", "1"}}, {}));
    ok = expect(
             missingValue.save.empty() && missingValue.forget.size() == 1,
             "an agent-owned secret without a value must keep what is stored"
         )
        && ok;
    return ok;
  }

  bool handlesWirelessSettings() {
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
    bool ok = expect(
        changes.save
            == std::vector<Secret>{{.id = {.setting = "802-11-wireless-security", .key = "psk"}, .value = "wifi-pass"}},
        "only the agent-owned PSK should be saved"
    );
    ok = expect(
             changes.forget == std::vector<SecretKey>{{.setting = "802-1x", .key = "password"}},
             "AGENT_OWNED with NOT_SAVED must not be kept"
         )
        && ok;
    return ok;
  }

  bool buildsReplies() {
    const auto vpn = nm_keyring::secretsReply("vpn", {{"password", "pw"}});
    bool ok = expect(
        vpn.at("vpn").at("secrets").get<StringMap>() == StringMap{{"password", "pw"}},
        "VPN secrets should be an a{ss} under vpn.secrets"
    );
    const auto wifi = nm_keyring::secretsReply("802-11-wireless-security", {{"psk", "pass"}});
    ok = expect(
             wifi.at("802-11-wireless-security").at("psk").get<std::string>() == "pass",
             "other secrets should be plain properties of their setting"
         )
        && ok;
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = savesAgentOwnedVpnSecrets() && ok;
  ok = readsFlagsFromConnectionNotReply() && ok;
  ok = handlesWirelessSettings() && ok;
  ok = buildsReplies() && ok;
  return ok ? 0 : 1;
}
