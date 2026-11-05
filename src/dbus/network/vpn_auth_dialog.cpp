#include "dbus/network/vpn_auth_dialog.h"

#include "system/freedesktop_key_file.h"

#include <algorithm>
#include <system_error>

namespace vpn_auth {

  namespace {

    constexpr std::string_view kConnectionGroup = "VPN Connection";
    constexpr std::string_view kGnomeGroup = "GNOME";
    constexpr std::string_view kUiGroup = "VPN Plugin UI";

    constexpr std::uint32_t kFlagAllowInteraction = 0x1;
    constexpr std::uint32_t kFlagRequestNew = 0x2;

    bool keyFileBool(const freedesktop::KeyFile& file, std::string_view group, std::string_view key) {
      const auto value = file.value(group, key);
      return value == "true" || value == "1";
    }

    std::string keyFileString(const freedesktop::KeyFile& file, std::string_view group, std::string_view key) {
      return freedesktop::unescapeValue(file.value(group, key).value_or(std::string_view{}));
    }

    bool namesService(const freedesktop::KeyFile& file, std::string_view serviceType) {
      if (file.value(kConnectionGroup, "service") == serviceType) {
        return true;
      }
      std::string_view aliases = file.value(kConnectionGroup, "aliases").value_or(std::string_view{});
      while (!aliases.empty()) {
        const std::size_t end = aliases.find(';');
        if (aliases.substr(0, end) == serviceType) {
          return true;
        }
        aliases.remove_prefix(end == std::string_view::npos ? aliases.size() : end + 1);
      }
      return false;
    }

    void appendEntries(std::string& out, std::string_view keyTag, std::string_view valueTag, const StringMap& entries) {
      for (const auto& [key, value] : entries) {
        out.append(keyTag).append("=").append(key).append("\n");
        out.append(valueTag).append("=").append(value).append("\n\n");
      }
    }

  } // namespace

  std::vector<std::filesystem::path> defaultPluginDirs() {
    // /etc first: NetworkManager lets it override the packaged plugins, and it is
    // where NixOS installs them.
    return {
        "/etc/NetworkManager/VPN",
        "/usr/lib/NetworkManager/VPN",
        "/usr/lib64/NetworkManager/VPN",
        "/usr/local/lib/NetworkManager/VPN",
    };
  }

  std::optional<PluginInfo> findPlugin(std::string_view serviceType, const std::vector<std::filesystem::path>& dirs) {
    if (serviceType.empty()) {
      return std::nullopt;
    }
    for (const auto& dir : dirs) {
      std::error_code ec;
      std::vector<std::filesystem::path> candidates;
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".name") {
          candidates.push_back(entry.path());
        }
      }
      std::ranges::sort(candidates);
      for (const auto& path : candidates) {
        const auto parsed = freedesktop::parseKeyFile(path);
        if (!parsed.has_value() || !namesService(parsed->file, serviceType)) {
          continue;
        }
        const auto& file = parsed->file;
        return PluginInfo{
            .service = std::string(file.value(kConnectionGroup, "service").value_or(serviceType)),
            .authDialog = std::string(file.value(kGnomeGroup, "auth-dialog").value_or(std::string_view{})),
            .externalUiMode = keyFileBool(file, kGnomeGroup, "supports-external-ui-mode"),
            .supportsHints = keyFileBool(file, kGnomeGroup, "supports-hints"),
        };
      }
    }
    return std::nullopt;
  }

  std::vector<std::string> dialogArgs(const PluginInfo& plugin, const DialogRequest& request) {
    std::vector<std::string> args{
        plugin.authDialog, "-u", request.connectionUuid, "-n", request.connectionId, "-s", request.serviceType,
    };
    if (plugin.externalUiMode) {
      args.emplace_back("--external-ui-mode");
    }
    if ((request.flags & kFlagAllowInteraction) != 0U) {
      args.emplace_back("-i");
    }
    if ((request.flags & kFlagRequestNew) != 0U) {
      args.emplace_back("-r");
    }
    if (plugin.supportsHints) {
      for (const auto& hint : request.hints) {
        args.emplace_back("-t");
        args.push_back(hint);
      }
    }
    return args;
  }

  std::string dialogInput(const DialogRequest& request) {
    std::string out;
    appendEntries(out, "DATA_KEY", "DATA_VAL", request.data);
    appendEntries(out, "SECRET_KEY", "SECRET_VAL", request.secrets);
    out.append("DONE\n\nQUIT\n\n");
    return out;
  }

  std::expected<Prompt, std::string> parseExternalUiOutput(std::string_view output) {
    if (output.find_first_not_of(" \t\r\n") == std::string_view::npos) {
      return Prompt{};
    }
    const auto parsed = freedesktop::parseKeyFileContents(output);
    if (!parsed.has_value()) {
      return std::unexpected(parsed.error().message);
    }
    const auto& file = parsed->file;
    if (file.value(kUiGroup, "Version") != "2") {
      return std::unexpected("missing or unsupported [VPN Plugin UI] version");
    }

    Prompt prompt{
        .title = keyFileString(file, kUiGroup, "Title"),
        .message = keyFileString(file, kUiGroup, "Description"),
    };
    // The group name is the key NetworkManager expects the answer under, hint
    // prefix and all (e.g. "x-dynamic-challenge-echo:challenge-response").
    for (const auto& group : file.sectionNames()) {
      if (group == kUiGroup) {
        continue;
      }
      std::string value = keyFileString(file, group, "Value");
      if (keyFileBool(file, group, "ShouldAsk")) {
        prompt.fields.push_back({
            .key = group,
            .label = keyFileString(file, group, "Label"),
            .value = std::move(value),
            .secret = keyFileBool(file, group, "IsSecret"),
            .echo = keyFileBool(file, group, "ForceEcho"),
        });
      } else if (!value.empty()) {
        prompt.known.insert_or_assign(group, std::move(value));
      }
    }
    return prompt;
  }

  StringMap parseLegacyOutput(std::string_view output) {
    StringMap secrets;
    while (!output.empty()) {
      const std::size_t keyEnd = output.find('\n');
      const std::string_view key = output.substr(0, keyEnd);
      if (key.empty() || keyEnd == std::string_view::npos) {
        break;
      }
      output.remove_prefix(keyEnd + 1);
      const std::size_t valueEnd = output.find('\n');
      secrets.insert_or_assign(std::string(key), std::string(output.substr(0, valueEnd)));
      output.remove_prefix(valueEnd == std::string_view::npos ? output.size() : valueEnd + 1);
    }
    return secrets;
  }

} // namespace vpn_auth
