#include "dbus/network/vpn_auth_dialog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "vpn_auth_dialog_test: {}", message);
    }
    return condition;
  }

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

  bool parsesExternalUiOutput() {
    const auto prompt = vpn_auth::parseExternalUiOutput(kChallengeOutput);
    bool ok = expect(prompt.has_value(), "external UI output did not parse");
    if (!prompt.has_value()) {
      return false;
    }
    ok = expect(prompt->title == "Authentication required", "title was not read") && ok;
    ok = expect(prompt->message == "Enter your OTP\nfor \\\"vpn\\\"", "description was not unescaped") && ok;
    ok = expect(prompt->fields.size() == 1, "only ShouldAsk groups should become fields") && ok;
    if (!prompt->fields.empty()) {
      const auto& field = prompt->fields.front();
      ok = expect(field.key == "x-dynamic-challenge-echo:challenge-response", "field key must be the group name") && ok;
      ok = expect(field.label == "Challenge response", "field label was not read") && ok;
      ok = expect(field.secret && field.echo, "secret/echo flags were not read") && ok;
    }
    ok = expect(
             prompt->known == vpn_auth::StringMap{{"password", "stored"}},
             "non-asked groups with a value should be known secrets, empty ones dropped"
         )
        && ok;
    return ok;
  }

  bool keepsFieldOrder() {
    const auto prompt = vpn_auth::parseExternalUiOutput(
        "[VPN Plugin UI]\nVersion=2\n[zeta]\nShouldAsk=true\n[alpha]\nShouldAsk=true\n"
    );
    return expect(
        prompt.has_value()
            && prompt->fields.size() == 2
            && prompt->fields[0].key == "zeta"
            && prompt->fields[1].key == "alpha",
        "fields were not kept in the helper's order"
    );
  }

  bool rejectsMalformedExternalUiOutput() {
    bool ok = expect(vpn_auth::parseExternalUiOutput("\n").has_value(), "empty output means nothing to ask");
    ok = expect(vpn_auth::parseExternalUiOutput("\n")->fields.empty(), "empty output should ask nothing") && ok;
    ok =
        expect(!vpn_auth::parseExternalUiOutput("garbage").has_value(), "output without a UI group was accepted") && ok;
    ok = expect(
             !vpn_auth::parseExternalUiOutput("[VPN Plugin UI]\nVersion=1\n").has_value(),
             "unsupported version was accepted"
         )
        && ok;
    return ok;
  }

  bool parsesLegacyOutput() {
    bool ok = expect(
        vpn_auth::parseLegacyOutput("password\nhunter2\ncookie\na=b\n\n\nignored\nvalue")
            == vpn_auth::StringMap{{"password", "hunter2"}, {"cookie", "a=b"}},
        "legacy pairs were not parsed up to the blank line"
    );
    // The process runner trims trailing newlines from captured output.
    ok = expect(
             vpn_auth::parseLegacyOutput("password\nhunter2") == vpn_auth::StringMap{{"password", "hunter2"}},
             "trimmed legacy output lost its last value"
         )
        && ok;
    ok = expect(vpn_auth::parseLegacyOutput("").empty(), "empty legacy output should yield nothing") && ok;
    return ok;
  }

  bool buildsDialogArgsAndInput() {
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
    bool ok = expect(vpn_auth::dialogArgs(plugin, request) == expectedArgs, "unexpected auth dialog arguments");

    auto legacy = plugin;
    legacy.externalUiMode = false;
    legacy.supportsHints = false;
    request.flags = 0x1;
    ok = expect(
             vpn_auth::dialogArgs(legacy, request)
                 == std::vector<std::string>{legacy.authDialog, "-u", "uuid", "-n", "Work", "-s", plugin.service, "-i"},
             "legacy helpers must not get external UI mode or hints"
         )
        && ok;

    ok = expect(
             vpn_auth::dialogInput(request)
                 == "DATA_KEY=connection-type\nDATA_VAL=password-tls\n\n"
                    "SECRET_KEY=password\nSECRET_VAL=pw\n\n"
                    "DONE\n\nQUIT\n\n",
             "unexpected auth dialog input"
         )
        && ok;
    return ok;
  }

  bool findsPluginByServiceOrAlias() {
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
    bool ok = expect(
        openvpn.has_value()
            && openvpn->authDialog == "/lib/auth-dialog"
            && openvpn->externalUiMode
            && openvpn->supportsHints,
        "plugin was not found by service name"
    );
    const auto alias = vpn_auth::findPlugin("org.example.ovpn", dirs);
    ok = expect(
             alias.has_value() && alias->service == "org.freedesktop.NetworkManager.openvpn",
             "plugin was not found by alias"
         )
        && ok;
    const auto vpnc = vpn_auth::findPlugin("org.freedesktop.NetworkManager.vpnc", dirs);
    ok = expect(
             vpnc.has_value() && vpnc->authDialog == "/etc/auth-dialog" && !vpnc->externalUiMode,
             "earlier directories should take priority"
         )
        && ok;
    ok = expect(!vpn_auth::findPlugin("org.example.ignored", dirs).has_value(), "non-.name file was read") && ok;
    ok = expect(!vpn_auth::findPlugin("", dirs).has_value(), "empty service type matched a plugin") && ok;

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = parsesExternalUiOutput() && ok;
  ok = keepsFieldOrder() && ok;
  ok = rejectsMalformedExternalUiOutput() && ok;
  ok = parsesLegacyOutput() && ok;
  ok = buildsDialogArgsAndInput() && ok;
  ok = findsPluginByServiceOrAlias() && ok;
  return ok ? 0 : 1;
}
