#pragma once

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace process {

  struct RunResult {
    int exitCode = -1;
    std::string out;
    std::string err;
    bool timedOut = false;
    bool outTruncated = false;
    bool errTruncated = false;

    operator bool() const { return exitCode == 0 && !timedOut; }
  };

  [[nodiscard]] bool commandExists(const char* name);

  // Shell string or argv — runs fully detached (double-fork + setsid) so the child is not a
  // direct subprocess of noctalia (hooks, idle commands, launcher parity).
  [[nodiscard]] bool runAsync(const std::string& command);
  [[nodiscard]] RunResult runSync(const std::string& command);

  // Arg vector — direct execvp; same detach semantics as runAsync(string). When activationToken is
  // non-empty, the grandchild sets XDG_ACTIVATION_TOKEN and DESKTOP_STARTUP_ID (launcher).
  [[nodiscard]] bool runAsync(
      const std::vector<std::string>& args, const std::string& activationToken = {}, const std::string& workingDir = {}
  );
  [[nodiscard]] bool runAsync(std::initializer_list<const char*> args);
  [[nodiscard]] RunResult runSync(const std::vector<std::string>& args);
  [[nodiscard]] RunResult runSync(std::initializer_list<const char*> args);
  [[nodiscard]] RunResult runSyncWithTimeout(const std::vector<std::string>& args, std::chrono::milliseconds timeout);
  [[nodiscard]] RunResult
  runSyncWithTimeout(std::initializer_list<const char*> args, std::chrono::milliseconds timeout);
  [[nodiscard]] RunResult runSyncWithTimeoutAndOutputLimit(
      const std::vector<std::string>& args, std::chrono::milliseconds timeout, std::size_t maxOutputBytes
  );
  [[nodiscard]] bool commandLineMatchesAll(const std::vector<std::string>& needles);
  [[nodiscard]] bool desktopPortalAvailable();
  [[nodiscard]] bool flatpakAppInstalled(std::string_view appId);

  // Like runAsync(args), but returns the grandchild pid for terminateTracked (optional API).
  [[nodiscard]] std::optional<int> launchDetachedTracked(const std::vector<std::string>& args);
  [[nodiscard]] std::optional<int> launchDetachedTracked(std::initializer_list<const char*> args);
  void terminateTracked(int pid);

  [[nodiscard]] bool launchFirstAvailable(std::initializer_list<std::initializer_list<const char*>> commandVariants);

  bool systemdAvailable();
  // Run an app as a systemd service
  void runAsyncAsSystemdService(
      const std::vector<std::string>& args, const std::string& appName, const std::string& activationToken = {},
      const std::string& workingDir = {}
  );
} // namespace process
