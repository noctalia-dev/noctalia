#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
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

  struct EnvOverride {
    std::string name;
    std::optional<std::string> value;
  };

  using OutputCallback = std::function<void(std::string_view chunk)>;
  using ExitCallback = std::function<void(RunResult result)>;

  struct RunCallbacks {
    OutputCallback stdOut;
    OutputCallback stdErr;
    ExitCallback onExit;
  };

  struct RunOptions {
    std::optional<std::chrono::milliseconds> timeout;
    std::size_t maxOutputBytes = std::numeric_limits<std::size_t>::max();
    // When set, the run is cancellable: once the flag turns true the child's
    // process group is terminated and the call returns. Lets a streaming, never-
    // exiting process (e.g. `evtest`) be stopped on reload/teardown.
    std::shared_ptr<std::atomic<bool>> cancel;
    // Child-only environment changes. `nullopt` unsets the variable.
    std::vector<EnvOverride> env;
  };

  [[nodiscard]] bool commandExists(const char* name);

  // Prefer run0 over pkexec: pkexec often stays on PATH without the setuid wrapper (e.g. NixOS).
  [[nodiscard]] std::optional<std::string> resolvePrivilegeEscalator();

  // Shell string — runs fully detached (double-fork + setsid) so the child is not a direct
  // subprocess of noctalia (hooks, idle commands, launcher parity).
  [[nodiscard]] bool runAsync(const std::string& command);
  // Shell string — runs on a worker thread, captures stdout/stderr, and invokes callbacks from
  // that worker. Output chunk views are valid only for the callback call.
  [[nodiscard]] bool runAsync(const std::string& command, RunCallbacks callbacks, RunOptions options = {});
  [[nodiscard]] RunResult runSync(const std::string& command);

  // Arg vector — direct execvp; same detach semantics as runAsync(string). When activationToken is
  // non-empty, the grandchild sets XDG_ACTIVATION_TOKEN and DESKTOP_STARTUP_ID (launcher).
  [[nodiscard]] bool runAsync(
      const std::vector<std::string>& args, const std::string& activationToken = {}, const std::string& workingDir = {}
  );
  // Arg vector — runs on a worker thread, captures stdout/stderr, and invokes callbacks from that
  // worker. Output chunk views are valid only for the callback call.
  [[nodiscard]] bool runAsync(const std::vector<std::string>& args, RunCallbacks callbacks, RunOptions options = {});
  [[nodiscard]] bool runAsync(std::initializer_list<const char*> args);
  [[nodiscard]] bool runAsync(std::initializer_list<const char*> args, RunCallbacks callbacks, RunOptions options = {});
  [[nodiscard]] RunResult runSync(const std::vector<std::string>& args);
  [[nodiscard]] RunResult runSync(const std::vector<std::string>& args, RunOptions options);
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

  [[nodiscard]] bool systemdAvailable();
  [[nodiscard]] bool runAsyncAsSystemdService(
      const std::vector<std::string>& args, const std::string& appName, const std::string& activationToken = {},
      const std::string& workingDir = {}
  );
} // namespace process
