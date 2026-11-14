#include "dbus/network/vpn_auth_dialog.h"
#include "tests/test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

  // Captured from nm-openvpn-auth-dialog --external-ui-mode after the server sent a
  // dynamic challenge.
  constexpr std::string_view kChallengeOutput = "[VPN Plugin UI]\n"
                                                "Version=2\n"
                                                "Description=Enter your OTP\\nfor \\\"vpn\\\"\n"
                                                "Title=Authentication required\n"
                                                "\n"
                                                "[password]\n"
                                                "Value=stored\n"
                                                "Label=Password\n"
                                                "IsSecret=true\n"
                                                "ShouldAsk=false\n"
                                                "ForceEcho=false\n"
                                                "\n"
                                                "[cert-pass]\n"
                                                "Value=\n"
                                                "Label=Certificate password\n"
                                                "IsSecret=true\n"
                                                "ShouldAsk=false\n"
                                                "ForceEcho=false\n"
                                                "\n"
                                                "[x-dynamic-challenge-echo:challenge-response]\n"
                                                "Value=\n"
                                                "Label=Challenge response\n"
                                                "IsSecret=true\n"
                                                "ShouldAsk=true\n"
                                                "ForceEcho=true\n";

  void parsesExternalUiOutput() {
    const auto prompt = vpn_auth::parseExternalUiOutput(kChallengeOutput);
    TEST_CHECK(prompt.has_value());
    TEST_CHECK(prompt->title == "Authentication required");
    // The description is unescaped.
    TEST_CHECK(prompt->message == "Enter your OTP\nfor \\\"vpn\\\"");
    // Only ShouldAsk groups become fields.
    TEST_CHECK(prompt->fields.size() == 1);
    const auto& field = prompt->fields.front();
    TEST_CHECK(field.key == "x-dynamic-challenge-echo:challenge-response");
    TEST_CHECK(field.label == "Challenge response");
    TEST_CHECK(field.secret && field.echo);
    // Non-asked groups with a value are known secrets, empty ones dropped.
    TEST_CHECK(prompt->known == vpn_auth::StringMap{{"password", "stored"}});
  }

  void keepsFieldOrder() {
    const auto prompt = vpn_auth::parseExternalUiOutput(
        "[VPN Plugin UI]\nVersion=2\n[zeta]\nShouldAsk=true\n[alpha]\nShouldAsk=true\n"
    );
    TEST_CHECK(
        prompt.has_value()
        && prompt->fields.size() == 2
        && prompt->fields[0].key == "zeta"
        && prompt->fields[1].key == "alpha"
    );
  }

  void rejectsMalformedExternalUiOutput() {
    // Empty output means nothing to ask.
    TEST_CHECK(vpn_auth::parseExternalUiOutput("\n").has_value());
    TEST_CHECK(vpn_auth::parseExternalUiOutput("\n")->fields.empty());
    // Output without a UI group is rejected.
    TEST_CHECK(!vpn_auth::parseExternalUiOutput("garbage").has_value());
    // Unsupported version is rejected.
    TEST_CHECK(!vpn_auth::parseExternalUiOutput("[VPN Plugin UI]\nVersion=1\n").has_value());
  }

  void parsesLegacyOutput() {
    // Legacy pairs are parsed up to the blank line.
    TEST_CHECK(
        vpn_auth::parseLegacyOutput("password\nhunter2\ncookie\na=b\n\n\nignored\nvalue")
        == vpn_auth::StringMap{{"password", "hunter2"}, {"cookie", "a=b"}}
    );
    // The process runner trims trailing newlines from captured output.
    TEST_CHECK(vpn_auth::parseLegacyOutput("password\nhunter2") == vpn_auth::StringMap{{"password", "hunter2"}});
    TEST_CHECK(vpn_auth::parseLegacyOutput("").empty());
  }

  void buildsDialogArgsAndInput() {
    const vpn_auth::PluginInfo plugin{
        .service = "org.freedesktop.NetworkManager.openvpn",
        .authDialog = "/usr/libexec/nm-openvpn-auth-dialog",
        .externalUiMode = true,
        .supportsHints = true,
    };
    vpn_auth::DialogRequest request{
        .connectionUuid = "uuid",
        .connectionId = "Work",
        .serviceType = plugin.service,
        .hints = {"x-vpn-message:Code?", "x-dynamic-challenge-echo:challenge-response"},
        .flags = 0x3,
        .data = {{"connection-type", "password-tls"}},
        .secrets = {{"password", "pw"}},
    };
    const std::vector<std::string> expectedArgs{
        "/usr/libexec/nm-openvpn-auth-dialog",
        "-u",
        "uuid",
        "-n",
        "Work",
        "-s",
        "org.freedesktop.NetworkManager.openvpn",
        "--external-ui-mode",
        "-i",
        "-r",
        "-t",
        "x-vpn-message:Code?",
        "-t",
        "x-dynamic-challenge-echo:challenge-response",
    };
    TEST_CHECK(vpn_auth::dialogArgs(plugin, request) == expectedArgs);

    auto legacy = plugin;
    legacy.externalUiMode = false;
    legacy.supportsHints = false;
    request.flags = 0x1;
    // Legacy helpers must not get external UI mode or hints.
    TEST_CHECK(
        vpn_auth::dialogArgs(legacy, request)
        == std::vector<std::string>{legacy.authDialog, "-u", "uuid", "-n", "Work", "-s", plugin.service, "-i"}
    );

    TEST_CHECK(
        vpn_auth::dialogInput(request)
        == "DATA_KEY=connection-type\nDATA_VAL=password-tls\n\n"
           "SECRET_KEY=password\nSECRET_VAL=pw\n\n"
           "DONE\n\nQUIT\n\n"
    );
  }

  void findsPluginByServiceOrAlias() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("noctalia-vpn-auth-dialog-test-" + std::to_string(now));
    const auto etc = root / "etc";
    const auto lib = root / "lib";
    std::filesystem::create_directories(etc);
    std::filesystem::create_directories(lib);
    {
      std::ofstream(lib / "nm-openvpn-service.name") << "[VPN Connection]\n"
                                                        "service=org.freedesktop.NetworkManager.openvpn\n"
                                                        "aliases=org.example.openvpn;org.example.ovpn\n"
                                                        "[GNOME]\n"
                                                        "auth-dialog=/lib/auth-dialog\n"
                                                        "supports-external-ui-mode=true\n"
                                                        "supports-hints=true\n";
      std::ofstream(etc / "override.name") << "[VPN Connection]\n"
                                              "service=org.freedesktop.NetworkManager.vpnc\n"
                                              "[GNOME]\n"
                                              "auth-dialog=/etc/auth-dialog\n";
      std::ofstream(lib / "nm-vpnc-service.name") << "[VPN Connection]\n"
                                                     "service=org.freedesktop.NetworkManager.vpnc\n"
                                                     "[GNOME]\n"
                                                     "auth-dialog=/lib/vpnc-auth-dialog\n";
      std::ofstream(lib / "ignored.txt") << "[VPN Connection]\nservice=org.example.ignored\n";
    }
    const std::vector<std::filesystem::path> dirs{etc, lib, root / "missing"};

    const auto openvpn = vpn_auth::findPlugin("org.freedesktop.NetworkManager.openvpn", dirs);
    const auto alias = vpn_auth::findPlugin("org.example.ovpn", dirs);
    const auto vpnc = vpn_auth::findPlugin("org.freedesktop.NetworkManager.vpnc", dirs);
    const auto ignored = vpn_auth::findPlugin("org.example.ignored", dirs);
    const auto empty = vpn_auth::findPlugin("", dirs);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    TEST_CHECK(
        openvpn.has_value()
        && openvpn->authDialog == "/lib/auth-dialog"
        && openvpn->externalUiMode
        && openvpn->supportsHints
    );
    TEST_CHECK(alias.has_value() && alias->service == "org.freedesktop.NetworkManager.openvpn");
    // Earlier directories take priority.
    TEST_CHECK(vpnc.has_value() && vpnc->authDialog == "/etc/auth-dialog" && !vpnc->externalUiMode);
    // Files without a .name extension are not read.
    TEST_CHECK(!ignored.has_value());
    TEST_CHECK(!empty.has_value());
  }

} // namespace

int main() {
  parsesExternalUiOutput();
  keepsFieldOrder();
  rejectsMalformedExternalUiOutput();
  parsesLegacyOutput();
  buildsDialogArgsAndInput();
  findsPluginByServiceOrAlias();
  return 0;
}
