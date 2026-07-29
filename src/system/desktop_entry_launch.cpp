#include "system/desktop_entry_launch.h"

#include "core/log.h"
#include "core/process/process.h"
#include "system/desktop_entry.h"
#include "system/terminal_launch.h"
#include "util/file_utils.h"

#include <chrono>
#include <map>
#include <mutex>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("desktop_entry_launch");

  std::string decodeDesktopStringValue(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\\' && i + 1 < value.size()) {
        const char next = value[i + 1];
        if (next == '\\' || next == 's' || next == 'n' || next == 't' || next == 'r') {
          // https://specifications.freedesktop.org/desktop-entry/latest/value-types.html
          // https://specifications.freedesktop.org/desktop-entry-spec/latest/exec-variables.html
          result += (next == 's') ? ' ' : (next == 'n') ? '\n' : (next == 't') ? '\t' : (next == 'r') ? '\r' : '\\';
          ++i;
          continue;
        }
        // Any other \X: remove the backslash, keep X
        result += next;
        ++i;
        continue;
      }
      result += value[i];
    }
    return result;
  }

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

    // Strip orphaned Flatpak @@ file-forwarding markers.
    for (std::size_t pos = 0;;) {
      pos = result.find("@@u", pos);
      if (pos == std::string::npos)
        break;
      std::size_t end = result.find("@@", pos + 3);
      if (end == std::string::npos)
        break;
      bool onlyWhitespace = true;
      for (std::size_t j = pos + 3; j < end; ++j) {
        if (result[j] != ' ') {
          onlyWhitespace = false;
          break;
        }
      }
      if (onlyWhitespace) {
        result.erase(pos, end + 2 - pos);
      } else {
        pos = end + 2;
      }
    }

    return result;
  }

  std::vector<std::string> tokenize(std::string_view cmd) {
    std::vector<std::string> args;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;

    for (std::size_t i = 0; i < cmd.size(); ++i) {
      const char c = cmd[i];
      if (c == '\\' && inDouble && i + 1 < cmd.size()) {
        const char next = cmd[i + 1];
        if (next == '"' || next == '`' || next == '$' || next == '\\') {
          current += next;
          ++i;
          continue;
        }
      }
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

  std::string parseCustomCommand(const std::string& exec, const std::string& customCommand) {
    if (customCommand.empty()) {
      return exec;
    }
    constexpr std::string_view kPlaceholder = "$CMD";
    if (!customCommand.contains(kPlaceholder)) {
      kLog.warn("Custom command does not contain '$CMD': '{}'", customCommand);
    }
    std::string command = customCommand;
    for (std::size_t pos = 0; (pos = command.find(kPlaceholder, pos)) != std::string::npos; pos += exec.length()) {
      command.replace(pos, kPlaceholder.size(), exec);
    }
    return command;
  }

  // Resolves whether this launch really goes through systemd, warning once when the option is set
  // but the shell is not itself a systemd user unit: apps would then be moved out of the login
  // session the shell lives in.
  bool effectiveRunAsSystemdService(const desktop_entry_launch::LaunchOptions& options) {
    if (!options.runAsSystemdService) {
      return false;
    }
    if (process::runningUnderSystemdUserManager()) {
      return true;
    }
    static std::once_flag warnOnce;
    std::call_once(warnOnce, [] {
      kLog.warn(
          "launch_apps_as_systemd_services is enabled but Noctalia is not running under the systemd user "
          "manager (no uwsm or systemd user service); launching apps directly"
      );
    });
    return false;
  }

  std::string dbusObjectPathForAppId(std::string_view appId) {
    std::string path;
    path.reserve(appId.size() + 1);
    path.push_back('/');
    for (const char c : appId) {
      path.push_back(c == '.' ? '/' : c);
    }
    return path;
  }

  std::map<std::string, sdbus::Variant> dbusPlatformData(const std::string& activationToken) {
    std::map<std::string, sdbus::Variant> platformData;
    if (!activationToken.empty()) {
      platformData.emplace("desktop-startup-id", sdbus::Variant{activationToken});
      platformData.emplace("activation-token", sdbus::Variant{activationToken});
    }
    return platformData;
  }

  // Prefer D-Bus only when the app is already on the bus (focus / hand off).
  bool tryDbusActivation(const desktop_entry_launch::LaunchOptions& options) {
    if (!options.dbusActivatable || options.dbusAppId.empty() || !options.customCommand.empty()) {
      return false;
    }

    try {
      auto connection = sdbus::createSessionBusConnection();
      constexpr auto kProbeTimeout = std::chrono::milliseconds(200);
      constexpr auto kActivateTimeout = std::chrono::milliseconds(500);
      constexpr auto kIface = "org.freedesktop.Application";

      auto dbus = sdbus::createProxy(
          *connection, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
      );
      bool hasOwner = false;
      dbus->callMethod("NameHasOwner")
          .onInterface("org.freedesktop.DBus")
          .withTimeout(kProbeTimeout)
          .withArguments(options.dbusAppId)
          .storeResultsTo(hasOwner);
      if (!hasOwner) {
        return false;
      }

      auto proxy = sdbus::createProxy(
          *connection, sdbus::ServiceName{options.dbusAppId},
          sdbus::ObjectPath{dbusObjectPathForAppId(options.dbusAppId)}
      );
      auto platformData = dbusPlatformData(options.activationToken);

      if (options.desktopActionId.empty()) {
        proxy->callMethod("Activate").onInterface(kIface).withTimeout(kActivateTimeout).withArguments(platformData);
      } else {
        proxy->callMethod("ActivateAction")
            .onInterface(kIface)
            .withTimeout(kActivateTimeout)
            .withArguments(options.desktopActionId, std::vector<sdbus::Variant>{}, platformData);
      }
      return true;
    } catch (const sdbus::Error& error) {
      kLog.debug(
          "dbus activation failed for '{}' (action='{}'): {}; falling back to Exec", options.dbusAppId,
          options.desktopActionId, error.what()
      );
      return false;
    } catch (const std::exception& error) {
      kLog.debug("dbus activation failed for '{}': {}; falling back to Exec", options.dbusAppId, error.what());
      return false;
    }
  }

  bool launchPrepared(
      const std::vector<std::string>& args, std::string_view appName, std::string_view workingDir,
      const desktop_entry_launch::LaunchOptions& options, bool runAsSystemdService
  ) {
    if (runAsSystemdService) {
      return process::runAsyncAsSystemdService(
          args, appNameOrDefault(appName), options.activationToken, std::string(workingDir)
      );
    }
    return process::runAsync(args, options.activationToken, std::string(workingDir));
  }

} // namespace

namespace desktop_entry_launch {

  std::optional<PreparedCommand> prepareCommand(std::string_view exec, bool terminal, const PrepareOptions& options) {
    const std::string decodedExec = decodeDesktopStringValue(exec);
    const std::string cleanExec = stripFieldCodes(decodedExec);
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
    LaunchOptions resolved = options;
    if (resolved.dbusAppId.empty()) {
      resolved.dbusAppId = entry.id;
    }
    if (!resolved.dbusActivatable) {
      resolved.dbusActivatable = entry.dbusActivatable;
    }
    resolved.desktopActionId.clear();

    if (tryDbusActivation(resolved)) {
      return true;
    }

    const bool runAsSystemdService = effectiveRunAsSystemdService(resolved);
    if (runAsSystemdService && !resolved.customCommand.empty()) {
      kLog.warn(
          "launch_apps_as_systemd_services and launch_apps_custom_command are mutually exclusive; ignoring custom "
          "command"
      );
    }
    const std::string customCommand = runAsSystemdService ? "" : resolved.customCommand;
    const std::string command = parseCustomCommand(entry.exec, customCommand);
    auto prepared = prepareCommand(command, entry.terminal);

    if (!prepared.has_value()) {
      kLog.warn("Failed to prepare launch command for desktop entry '{}'", entry.id.empty() ? entry.name : entry.id);
      return false;
    }

    const std::string appName = !entry.id.empty() ? entry.id : appNameOrDefault(entry.name);
    return launchPrepared(prepared->args, appName, entry.workingDir, resolved, runAsSystemdService);
  }

  bool launchAction(
      const DesktopAction& action, std::string_view appName, std::string_view workingDir, bool terminal,
      const LaunchOptions& options
  ) {
    LaunchOptions resolved = options;
    if (resolved.dbusAppId.empty()) {
      resolved.dbusAppId = std::string(appName);
    }
    resolved.desktopActionId = action.id;

    if (tryDbusActivation(resolved)) {
      return true;
    }

    const bool runAsSystemdService = effectiveRunAsSystemdService(resolved);
    if (runAsSystemdService && !resolved.customCommand.empty()) {
      kLog.warn(
          "launch_apps_as_systemd_services and launch_apps_custom_command are mutually exclusive; ignoring custom "
          "command"
      );
    }
    const std::string customCommand = runAsSystemdService ? "" : resolved.customCommand;
    const std::string command = parseCustomCommand(action.exec, customCommand);
    auto prepared = prepareCommand(command, terminal);
    if (!prepared.has_value()) {
      kLog.warn(
          "Failed to prepare launch command for desktop action '{}'", action.id.empty() ? action.name : action.id
      );
      return false;
    }

    return launchPrepared(prepared->args, appName, workingDir, resolved, runAsSystemdService);
  }

} // namespace desktop_entry_launch
