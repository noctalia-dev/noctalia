#include "system/desktop_entry_launch.h"

#include "core/log.h"
#include "core/process.h"
#include "system/desktop_entry.h"
#include "system/terminal_launch.h"
#include "util/file_utils.h"

#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("desktop_entry_launch");

  std::string stripFieldCodes(std::string_view exec) {
    std::string result;
    result.reserve(exec.size());
    for (std::size_t i = 0; i < exec.size(); ++i) {
      if (exec[i] == '%' && i + 1 < exec.size()) {
        const char next = exec[i + 1];
        if (next == 'f'
            || next == 'F'
            || next == 'u'
            || next == 'U'
            || next == 'd'
            || next == 'D'
            || next == 'n'
            || next == 'N'
            || next == 'i'
            || next == 'c'
            || next == 'k') {
          ++i;
          if (i + 1 < exec.size() && exec[i + 1] == ' ') {
            ++i;
          }
          continue;
        }
        if (next == '%') {
          result += '%';
          ++i;
          continue;
        }
      }
      result += exec[i];
    }

    while (!result.empty() && result.back() == ' ') {
      result.pop_back();
    }
    return result;
  }

  std::vector<std::string> tokenize(std::string_view cmd) {
    std::vector<std::string> args;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;

    for (const char c : cmd) {
      if (c == '\'' && !inDouble) {
        inSingle = !inSingle;
        continue;
      }
      if (c == '"' && !inSingle) {
        inDouble = !inDouble;
        continue;
      }
      if (c == ' ' && !inSingle && !inDouble) {
        if (!current.empty()) {
          args.push_back(std::move(current));
          current.clear();
        }
        continue;
      }
      current += c;
    }
    if (!current.empty()) {
      args.push_back(std::move(current));
    }
    return args;
  }

  std::string expandExecutablePath(std::string_view binary) {
    if (binary.empty() || binary.front() != '~') {
      return std::string(binary);
    }
    return FileUtils::expandUserPath(std::string(binary)).string();
  }

  std::string appNameOrDefault(std::string_view appName) {
    return appName.empty() ? "desktop-entry" : std::string(appName);
  }

} // namespace

namespace desktop_entry_launch {

  std::optional<PreparedCommand> prepareCommand(std::string_view exec, bool terminal, const PrepareOptions& options) {
    std::string cleanExec = stripFieldCodes(exec);
    std::vector<std::string> args;
    if (terminal) {
      auto prepared = terminal_launch::prepareCommand(
          cleanExec,
          terminal_launch::Options{
              .terminalCandidates = options.terminalCandidates,
              .useSystemTerminalDiscovery = options.useSystemTerminalDiscovery,
          }
      );
      if (!prepared.has_value()) {
        return std::nullopt;
      }
      args = std::move(*prepared);
    } else {
      args = tokenize(cleanExec);
    }

    if (!args.empty() && args.front().contains('/')) {
      args.front() = expandExecutablePath(args.front());
    }

    if (args.empty()) {
      return std::nullopt;
    }
    return PreparedCommand{std::move(args)};
  }

  bool launchEntry(const DesktopEntry& entry, const LaunchOptions& options) {
    auto prepared = prepareCommand(entry.exec, entry.terminal);
    if (!prepared.has_value()) {
      kLog.warn("Failed to prepare launch command for desktop entry '{}'", entry.id.empty() ? entry.name : entry.id);
      return false;
    }

    const std::string appName = !entry.id.empty() ? entry.id : appNameOrDefault(entry.name);
    if (options.runAsSystemdService) {
      return process::runAsyncAsSystemdService(prepared->args, appName, options.activationToken, entry.workingDir);
    }
    return process::runAsync(prepared->args, options.activationToken, entry.workingDir);
  }

  bool launchAction(
      const DesktopAction& action, std::string_view appName, std::string_view workingDir, bool terminal,
      const LaunchOptions& options
  ) {
    auto prepared = prepareCommand(action.exec, terminal);
    if (!prepared.has_value()) {
      kLog.warn(
          "Failed to prepare launch command for desktop action '{}'", action.id.empty() ? action.name : action.id
      );
      return false;
    }

    if (options.runAsSystemdService) {
      return process::runAsyncAsSystemdService(
          prepared->args, appNameOrDefault(appName), options.activationToken, std::string(workingDir)
      );
    }
    return process::runAsync(prepared->args, options.activationToken, std::string(workingDir));
  }

} // namespace desktop_entry_launch
