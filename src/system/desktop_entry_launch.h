#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct DesktopAction;
struct DesktopEntry;

namespace desktop_entry_launch {

  struct LaunchOptions {
    std::string activationToken;
    bool runAsSystemdService = false;
    std::string customCommand;
  };

  struct PrepareOptions {
    std::vector<std::string> terminalCandidates;
    bool useSystemTerminalDiscovery = true;
  };

  struct PreparedCommand {
    std::vector<std::string> args;
  };

  [[nodiscard]] std::optional<PreparedCommand>
  prepareCommand(std::string_view exec, bool terminal, const PrepareOptions& options = {});

  [[nodiscard]] bool launchEntry(const DesktopEntry& entry, const LaunchOptions& options = {});

  [[nodiscard]] bool launchAction(
      const DesktopAction& action, std::string_view appName, std::string_view workingDir, bool terminal,
      const LaunchOptions& options = {}
  );

} // namespace desktop_entry_launch
