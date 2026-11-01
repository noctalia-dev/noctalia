#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// NetworkManager VPN plugins ship an "auth dialog" helper that knows which
// secrets the plugin needs and how to label them. This is the client side of
// that helper's stdin/stdout protocol, as GNOME Shell and nm-applet speak it:
//
//   - The helper is found through the plugin's .name file ([GNOME] auth-dialog).
//   - It reads the connection's vpn.data and vpn.secrets on stdin, ending with DONE.
//   - In external UI mode it prints a key file describing the fields to ask for,
//     leaving the prompt to us. Older helpers instead show their own window, print
//     "key\nvalue\n" pairs and a blank line, and wait for QUIT on stdin.
namespace vpn_auth {

  using StringMap = std::map<std::string, std::string>;

  struct PluginInfo {
    std::string service;
    std::string authDialog;
    bool externalUiMode = false;
    bool supportsHints = false;
  };

  // Directories NetworkManager loads VPN plugin .name files from, highest priority first.
  [[nodiscard]] std::vector<std::filesystem::path> defaultPluginDirs();

  // Finds the plugin whose service name or one of its aliases is `serviceType`.
  [[nodiscard]] std::optional<PluginInfo>
  findPlugin(std::string_view serviceType, const std::vector<std::filesystem::path>& dirs);

  struct DialogRequest {
    std::string connectionUuid;
    std::string connectionId;
    std::string serviceType;
    std::vector<std::string> hints;
    // NMSecretAgentGetSecretsFlags as received in GetSecrets.
    std::uint32_t flags = 0;
    StringMap data;
    StringMap secrets;
  };

  [[nodiscard]] std::vector<std::string> dialogArgs(const PluginInfo& plugin, const DialogRequest& request);

  // Full stdin for the helper. QUIT follows DONE straight away: helpers read their
  // input one byte at a time and stop at DONE, so the QUIT that legacy helpers wait
  // for after printing is already queued, and external UI helpers never read it.
  [[nodiscard]] std::string dialogInput(const DialogRequest& request);

  struct Field {
    std::string key;
    std::string label;
    std::string value;
    bool secret = true;
    // Shown as typed even though it is a secret, e.g. a one-time code.
    bool echo = false;
  };

  struct Prompt {
    std::string title;
    std::string message;
    // Fields the user has to fill in, in the helper's order.
    std::vector<Field> fields;
    // Secrets the helper already knows and does not need to ask for.
    StringMap known;
  };

  // Parses an external UI mode helper's output. Empty output is valid and means
  // nothing needs asking.
  [[nodiscard]] std::expected<Prompt, std::string> parseExternalUiOutput(std::string_view output);

  // Parses a legacy helper's "key\nvalue\n" pairs, which end at the first blank key.
  [[nodiscard]] StringMap parseLegacyOutput(std::string_view output);

} // namespace vpn_auth
