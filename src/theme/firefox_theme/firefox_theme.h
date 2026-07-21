#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace noctalia::theme {

  inline constexpr std::string_view kFirefoxThemePostAction = "firefox-theme";

  struct FirefoxThemeApplyResult {
    bool success = false;
    std::string error;
    std::string warning;
  };

  // After a template writes colors.json: ensure the Pywalfox-compatible native-messaging
  // manifest points at this noctalia binary (without clobbering a foreign host), then push
  // theme mode (when dark/light) and a colors update to every running host — same parity as
  // the old community apply.sh (`pywalfox dark|light` + `pywalfox update`), with multi-profile
  // fan-out so all open Firefox profiles refresh.
  [[nodiscard]] FirefoxThemeApplyResult
  applyFirefoxTheme(const std::filesystem::path& colorsJsonPath, std::string_view mode = {});

  // True when argv looks like a Firefox/Thunderbird native-messaging launch of this process.
  [[nodiscard]] bool isFirefoxNativeMessagingLaunch(int argc, char* argv[]);

  // Stdio native-messaging host loop (length-prefixed JSON + unix socket + fan-out).
  // One host per Firefox profile; theme pushes reach all via a shared command file.
  int runFirefoxNativeMessagingHost();

  // CLI: noctalia firefox-theme <install|uninstall|update|dark|light|auto|host|...>
  int runFirefoxThemeCli(int argc, char* argv[]);

} // namespace noctalia::theme
