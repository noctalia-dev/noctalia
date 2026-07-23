#include "core/process/process.h"

#include "core/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/poll.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {

  constexpr Logger kLog("process");

  constexpr std::chrono::milliseconds kProcessPollInterval{100};
  constexpr std::chrono::milliseconds kProcessCommandLineCacheTtl{250};

  void writePipeOrIgnore(int fd, const void* data, size_t len) {
    auto p = reinterpret_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
      const ssize_t n = ::write(fd, p, remaining);
      if (n > 0) {
        p += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
      } else if (n == 0) {
        return;
      } else if (errno != EINTR) {
        return;
      }
    }
  }

  void attachStdioToDevNull() {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        ::close(devnull);
      }
    }
  }

  void closeFd(int& fd) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  void closePipe(int (&pipeFds)[2]) {
    closeFd(pipeFds[0]);
    closeFd(pipeFds[1]);
  }

  void trimTrailingLineEndings(std::string& value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
      value.pop_back();
    }
  }

  [[nodiscard]] std::optional<std::vector<std::string>> makeCommand(std::initializer_list<const char*> args) {
    std::vector<std::string> command;
    command.reserve(args.size());
    for (const char* arg : args) {
      if (arg == nullptr) {
        return std::nullopt;
      }
      command.emplace_back(arg);
    }
    return command;
  }

  [[nodiscard]] std::vector<char*> makeArgv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
  }

  [[nodiscard]] bool validEnvName(std::string_view name) { return !name.empty() && !name.contains('='); }

  void applyEnvOverrides(const std::vector<process::EnvOverride>& env) {
    for (const auto& item : env) {
      if (!validEnvName(item.name)) {
        continue;
      }
      if (item.value.has_value()) {
        ::setenv(item.name.c_str(), item.value->c_str(), 1);
      } else {
        ::unsetenv(item.name.c_str());
      }
    }
  }

  [[nodiscard]] bool isSafeFlatpakAppId(std::string_view appId) {
    if (appId.empty() || appId.contains('/') || appId.contains('\\')) {
      return false;
    }
    return !appId.contains("..");
  }

  void appendFlatpakDataRoots(std::vector<std::filesystem::path>& roots) {
    const char* home = std::getenv("HOME");
    const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
    if (xdgDataHome != nullptr && xdgDataHome[0] != '\0') {
      roots.emplace_back(xdgDataHome);
    } else if (home != nullptr && home[0] != '\0') {
      roots.emplace_back(std::filesystem::path(home) / ".local/share");
    }

    const char* xdgDataDirs = std::getenv("XDG_DATA_DIRS");
    std::string dirs = (xdgDataDirs != nullptr && xdgDataDirs[0] != '\0') ? xdgDataDirs : "/usr/local/share:/usr/share";
    std::stringstream ss(dirs);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
      if (!dir.empty()) {
        roots.emplace_back(dir);
      }
    }

    roots.emplace_back("/var/lib");
  }

  [[nodiscard]] bool isProcPidName(std::string_view name) {
    if (name.empty()) {
      return false;
    }
    return std::ranges::all_of(name, [](char ch) { return ch >= '0' && ch <= '9'; });
  }

  [[nodiscard]] std::string readProcCmdline(const std::filesystem::path& path) {
    std::ifstream file(path / "cmdline", std::ios::binary);
    if (!file) {
      return {};
    }

    std::string cmdline{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (cmdline.empty()) {
      return {};
    }

    for (char& ch : cmdline) {
      if (ch == '\0') {
        ch = ' ';
      }
    }
    while (!cmdline.empty() && cmdline.back() == ' ') {
      cmdline.pop_back();
    }
    if (cmdline.empty()) {
      return {};
    }

    return ' ' + cmdline + ' ';
  }

  [[nodiscard]] std::vector<std::string> readProcessCommandLines() {
    std::vector<std::string> commandLines;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator("/proc", std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_directory(ec) || ec) {
        ec.clear();
        continue;
      }
      const auto name = entry.path().filename().string();
      if (!isProcPidName(name)) {
        continue;
      }
      auto cmdline = readProcCmdline(entry.path());
      if (!cmdline.empty()) {
        commandLines.push_back(std::move(cmdline));
      }
    }
    return commandLines;
  }

  struct ProcessCommandLineCache {
    std::chrono::steady_clock::time_point capturedAt;
    std::vector<std::string> commandLines;
  };

  ProcessCommandLineCache& processCommandLineCache() {
    static ProcessCommandLineCache cache;
    return cache;
  }

  std::mutex& processCommandLineCacheMutex() {
    static std::mutex mutex;
    return mutex;
  }

  [[nodiscard]] std::vector<std::string> cachedProcessCommandLines() {
    std::scoped_lock lock(processCommandLineCacheMutex());
    auto& cache = processCommandLineCache();
    const auto now = std::chrono::steady_clock::now();
    if (cache.capturedAt.time_since_epoch().count() == 0 || now - cache.capturedAt >= kProcessCommandLineCacheTtl) {
      cache.commandLines = readProcessCommandLines();
      cache.capturedAt = now;
    }
    return cache.commandLines;
  }

  [[nodiscard]] bool cachedProcessMatchesAny(std::initializer_list<std::string_view> needles) {
    const auto& commandLines = cachedProcessCommandLines();
    return std::ranges::any_of(commandLines, [needles](const auto& commandLine) {
      return std::ranges::any_of(needles, [&commandLine](std::string_view needle) {
        return commandLine.find(needle) != std::string::npos;
      });
    });
  }

  bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  void emitOutputCallback(const process::OutputCallback* callback, const char* data, std::size_t len) {
    if (callback == nullptr || !*callback || len == 0) {
      return;
    }

    try {
      (*callback)(std::string_view(data, len));
    } catch (...) {
    }
  }

  void drainAvailable(
      int& fd, std::string& out, std::size_t maxBytes = std::numeric_limits<std::size_t>::max(),
      bool* truncated = nullptr, const process::OutputCallback* outputCallback = nullptr
  ) {
    if (fd < 0) {
      return;
    }

    char tmp[4096];
    for (;;) {
      const ssize_t n = ::read(fd, tmp, sizeof(tmp));
      if (n > 0) {
        const auto bytesRead = static_cast<std::size_t>(n);
        emitOutputCallback(outputCallback, tmp, bytesRead);

        const auto remaining = out.size() < maxBytes ? maxBytes - out.size() : 0;
        const auto appendBytes = std::min(bytesRead, remaining);
        if (appendBytes > 0) {
          out.append(tmp, appendBytes);
        }
        if (appendBytes < bytesRead && truncated != nullptr) {
          *truncated = true;
        }
        continue;
      }
      if (n == 0) {
        closeFd(fd);
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      closeFd(fd);
      return;
    }
  }

  [[nodiscard]] int exitCodeFromStatus(int status) {
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
      return 128 + WTERMSIG(status);
    }
    return -1;
  }

  [[nodiscard]] bool waitNoHang(pid_t pid, int& exitCode) {
    int status = 0;
    for (;;) {
      const pid_t result = ::waitpid(pid, &status, WNOHANG);
      if (result == pid) {
        exitCode = exitCodeFromStatus(status);
        return true;
      }
      if (result == 0) {
        return false;
      }
      if (errno == EINTR) {
        continue;
      }
      exitCode = -1;
      return true;
    }
  }

  [[nodiscard]] int waitBlocking(pid_t pid) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) {
        return -1;
      }
    }
    return exitCodeFromStatus(status);
  }

  void terminateAndWait(pid_t pid, int& exitCode) {
    // Signal the whole process group (child is its own group leader) so
    // descendants spawned by the command are torn down too, not orphaned.
    // Also signal the pid directly as a fallback in case setpgid did not take.
    ::kill(-pid, SIGTERM);
    ::kill(pid, SIGTERM);
    bool reaped = false;
    for (int i = 0; i < 10; ++i) {
      if (waitNoHang(pid, exitCode)) {
        reaped = true;
        break;
      }
      ::poll(nullptr, 0, 10);
    }

    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    if (!reaped) {
      exitCode = waitBlocking(pid);
    }
  }

  [[nodiscard]] int pollTimeoutMs(std::optional<std::chrono::steady_clock::time_point> deadline) {
    if (!deadline.has_value()) {
      return static_cast<int>(kProcessPollInterval.count());
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline) {
      return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
    const auto bounded = std::clamp(remaining, std::chrono::milliseconds(1), kProcessPollInterval);
    return static_cast<int>(bounded.count());
  }

  process::RunResult runSyncProcess(
      const std::vector<std::string>& args, const process::RunOptions& options,
      const process::RunCallbacks* callbacks = nullptr
  ) {
    if (args.empty() || args.front().empty()) {
      return {-1, {}, {}};
    }

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::pipe(outPipe) != 0) {
      return {-1, {}, {}};
    }
    if (::pipe(errPipe) != 0) {
      closePipe(outPipe);
      return {-1, {}, {}};
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
      closePipe(outPipe);
      closePipe(errPipe);
      return {-1, {}, {}};
    }

    if (pid == 0) {
      // Lead a new process group so a timeout can signal the whole tree
      // (e.g. checkupdates spawning pacman -Sy), not just the top-level shell.
      ::setpgid(0, 0);
      closeFd(outPipe[0]);
      closeFd(errPipe[0]);
      ::dup2(outPipe[1], STDOUT_FILENO);
      ::dup2(errPipe[1], STDERR_FILENO);
      closeFd(outPipe[1]);
      closeFd(errPipe[1]);

      applyEnvOverrides(options.env);
      std::vector<char*> argv = makeArgv(args);

      ::execvp(argv[0], argv.data());
      ::_exit(127);
    }

    // Mirror the child's setpgid so the group exists regardless of which side
    // wins the race; harmless if the child has already exec'd.
    ::setpgid(pid, pid);

    closeFd(outPipe[1]);
    closeFd(errPipe[1]);
    (void)setNonBlocking(outPipe[0]);
    (void)setNonBlocking(errPipe[0]);

    std::string out;
    std::string err;
    bool exited = false;
    bool timedOut = false;
    bool outTruncated = false;
    bool errTruncated = false;
    int exitCode = -1;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (options.timeout.has_value()) {
      deadline = std::chrono::steady_clock::now() + std::max(*options.timeout, std::chrono::milliseconds(0));
    }
    const auto* stdOutCallback = callbacks != nullptr ? &callbacks->stdOut : nullptr;
    const auto* stdErrCallback = callbacks != nullptr ? &callbacks->stdErr : nullptr;

    for (;;) {
      drainAvailable(outPipe[0], out, options.maxOutputBytes, &outTruncated, stdOutCallback);
      drainAvailable(errPipe[0], err, options.maxOutputBytes, &errTruncated, stdErrCallback);

      if (!exited) {
        exited = waitNoHang(pid, exitCode);
      }
      if (exited) {
        drainAvailable(outPipe[0], out, options.maxOutputBytes, &outTruncated, stdOutCallback);
        drainAvailable(errPipe[0], err, options.maxOutputBytes, &errTruncated, stdErrCallback);
        closeFd(outPipe[0]);
        closeFd(errPipe[0]);
        break;
      }

      if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
        timedOut = true;
        terminateAndWait(pid, exitCode);
      }

      // Cancellation: terminate the child and reuse the timed-out drain+break path.
      if (!timedOut && options.cancel && options.cancel->load(std::memory_order_relaxed)) {
        terminateAndWait(pid, exitCode);
        timedOut = true;
      }

      if (timedOut) {
        drainAvailable(outPipe[0], out, options.maxOutputBytes, &outTruncated, stdOutCallback);
        drainAvailable(errPipe[0], err, options.maxOutputBytes, &errTruncated, stdErrCallback);
        closeFd(outPipe[0]);
        closeFd(errPipe[0]);
        break;
      }

      std::array<pollfd, 2> fds{};
      nfds_t count = 0;
      if (outPipe[0] >= 0) {
        fds[count++] = pollfd{.fd = outPipe[0], .events = POLLIN, .revents = 0};
      }
      if (errPipe[0] >= 0) {
        fds[count++] = pollfd{.fd = errPipe[0], .events = POLLIN, .revents = 0};
      }

      if (count > 0) {
        int waitMs = exited ? 0 : pollTimeoutMs(deadline);
        // Cap the wait when cancellable so an idle stream still wakes to check the flag.
        if (options.cancel && (waitMs < 0 || waitMs > 250)) {
          waitMs = 250;
        }
        if (::poll(fds.data(), count, waitMs) < 0 && errno != EINTR) {
          break;
        }
      } else if (!exited) {
        if (!deadline.has_value()) {
          exitCode = waitBlocking(pid);
          exited = true;
          continue;
        }
        ::poll(nullptr, 0, std::min(10, pollTimeoutMs(deadline)));
      }
    }

    closeFd(outPipe[0]);
    closeFd(errPipe[0]);
    trimTrailingLineEndings(out);
    trimTrailingLineEndings(err);
    return {exitCode, std::move(out), std::move(err), timedOut, outTruncated, errTruncated};
  }

  [[nodiscard]] bool hasAnyCallback(const process::RunCallbacks& callbacks) {
    return callbacks.stdOut || callbacks.stdErr || callbacks.onExit;
  }

  // Double-fork + setsid so the exec'd process is not a direct child of the caller (matches
  // launcher app activation). Parent reaps the short-lived intermediate child.
  bool doubleForkExecDetached(
      const std::vector<std::string>& args, pid_t* reportPid, const std::string& activationToken,
      const std::string& workingDir = {}
  ) {
    int reportPipe[2] = {-1, -1};
    const bool needPid = reportPid != nullptr;
    if (needPid && ::pipe(reportPipe) != 0) {
      return false;
    }

    const pid_t intermediate = ::fork();
    if (intermediate < 0) {
      if (needPid) {
        ::close(reportPipe[0]);
        ::close(reportPipe[1]);
      }
      return false;
    }

    if (intermediate > 0) {
      // Parent: read grandchild pid first (intermediate may exit before the grandchild writes).
      if (needPid) {
        ::close(reportPipe[1]);
        pid_t reported = -1;
        const auto n = ::read(reportPipe[0], &reported, sizeof(reported));
        ::close(reportPipe[0]);
        int status = 0;
        while (::waitpid(intermediate, &status, 0) < 0 && errno == EINTR) {
        }
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && n == sizeof(reported) && reported > 0;
        if (ok) {
          *reportPid = reported;
        }
        return ok;
      }

      int status = 0;
      while (::waitpid(intermediate, &status, 0) < 0 && errno == EINTR) {
      }
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    // Intermediate child: new session, then fork again so the grandchild reparents away.
    if (needPid) {
      ::close(reportPipe[0]);
    }

    if (::setsid() < 0) {
      if (needPid) {
        const pid_t err = -1;
        writePipeOrIgnore(reportPipe[1], &err, sizeof(err));
        ::close(reportPipe[1]);
      }
      ::_exit(1);
    }

    const pid_t worker = ::fork();
    if (worker < 0) {
      if (needPid) {
        const pid_t err = -1;
        writePipeOrIgnore(reportPipe[1], &err, sizeof(err));
        ::close(reportPipe[1]);
      }
      ::_exit(1);
    }
    if (worker > 0) {
      if (needPid) {
        ::close(reportPipe[1]);
      }
      ::_exit(0);
    }

    // Grandchild
    if (needPid) {
      const pid_t self = ::getpid();
      writePipeOrIgnore(reportPipe[1], &self, sizeof(self));
      ::close(reportPipe[1]);
    }

    if (!workingDir.empty() && ::chdir(workingDir.c_str()) != 0) {
      ::_exit(126);
    }

    if (!activationToken.empty()) {
      ::setenv("XDG_ACTIVATION_TOKEN", activationToken.c_str(), 1);
      ::setenv("DESKTOP_STARTUP_ID", activationToken.c_str(), 1);
    }

    attachStdioToDevNull();

    std::vector<char*> argv = makeArgv(args);

    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }

  // Only alphanum, ':', '_' and '.' allowed in systemd unit names
  std::string escapeSystemdUnitName(const std::string& input) {
    std::string res;
    for (const unsigned char c : input) {
      if ((c >= 'A' && c <= 'Z')
          || (c >= 'a' && c <= 'z')
          || (c >= '0' && c <= '9')
          || c == ':'
          || c == '_'
          || c == '.') {
        res += c;
      } else {
        res += std::format("\\x{:02x}", c);
      }
    }
    return res;
  }

  bool startSystemdService(
      const std::vector<std::string>& args, const std::string& activationToken, const std::string& workingDir,
      const std::string& appName
  ) {
    std::vector<std::string> systemdArgs;
    systemdArgs.emplace_back("systemd-run");
    systemdArgs.emplace_back("--user");
    systemdArgs.emplace_back("--slice=app.slice");
    // Only end the service when all subprocesses have exited. Otherwise, apps using a launcher
    // script, e.g. vscode, would end prematurely when the script exits, and the actual app process
    // is still running.
    systemdArgs.emplace_back("--property=ExitType=cgroup");

    // We launch the app as a systemd service instead of a scope so the user can:
    // 1. Place drop-in files in ~/.config/systemd/user/app-<desktop-id>@.service.d/ to set properties like resource
    // limits or env vars.
    // 2. See the app's output and exit code (if it fails) in `systemctl status`.
    if (!appName.empty()) {
      const std::string uuid = StringUtils::generateUuid();
      if (!uuid.empty()) {
        systemdArgs.push_back(std::format("--unit=app-{}@{}.service", escapeSystemdUnitName(appName), uuid));
      }
    }
    if (!workingDir.empty()) {
      systemdArgs.push_back("--working-directory=" + workingDir);
    }

    process::RunOptions runOptions;

    if (!activationToken.empty()) {
      systemdArgs.emplace_back("-E");
      systemdArgs.emplace_back("XDG_ACTIVATION_TOKEN");
      systemdArgs.emplace_back("-E");
      systemdArgs.emplace_back("DESKTOP_STARTUP_ID");
      runOptions.env.push_back({"XDG_ACTIVATION_TOKEN", activationToken});
      runOptions.env.push_back({"DESKTOP_STARTUP_ID", activationToken});
    }

    // App should inherit our environment.
    char** s = ::environ;
    for (; *s; s++) {
      char c = **s;
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
        continue;
      }
      std::string name{*s, std::string_view(*s).find('=')};
      systemdArgs.emplace_back("-E");
      systemdArgs.push_back(name);
    }

    systemdArgs.emplace_back("--");
    systemdArgs.insert(systemdArgs.end(), args.begin(), args.end());
    return process::runAsync(
        systemdArgs,
        process::RunCallbacks{
            .stdOut = [](std::string_view) {},
            .stdErr = [](std::string_view) {},
            .onExit =
                [](process::RunResult result) {
                  if (result.exitCode != 0) {
                    // We'd like to show a toast or notification here, but the callback is not on the UI thread, so
                    // unclear how to
                    kLog.error(
                        "startSystemdService: systemd-run failed with exit code {}: {}", result.exitCode, result.err
                    );
                  }
                }
        },
        runOptions
    );
  }
} // namespace

namespace process {

  std::optional<std::string> resolvePrivilegeEscalator() {
    if (commandExists("run0")) {
      return "run0";
    }
    if (commandExists("pkexec")) {
      return "pkexec";
    }
    return std::nullopt;
  }

  bool commandExists(const char* name) {
    if (name == nullptr || name[0] == '\0') {
      return false;
    }

    if (std::strchr(name, '/') != nullptr) {
      std::error_code ec;
      return ::access(name, X_OK) == 0 && std::filesystem::is_regular_file(name, ec);
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0') {
      pathEnv = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    }

    std::string_view path(pathEnv);
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = path.find(':', start);
      const std::string_view dir = end == std::string_view::npos ? path.substr(start) : path.substr(start, end - start);
      const std::filesystem::path candidate =
          dir.empty() ? std::filesystem::path(name) : (std::filesystem::path(dir) / name);
      std::error_code ec;
      if (::access(candidate.c_str(), X_OK) == 0 && std::filesystem::is_regular_file(candidate, ec)) {
        return true;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }

    return false;
  }

  bool
  runAsync(const std::vector<std::string>& args, const std::string& activationToken, const std::string& workingDir) {
    if (args.empty() || args.front().empty()) {
      return false;
    }
    return doubleForkExecDetached(args, nullptr, activationToken, workingDir);
  }

  bool runAsync(const std::vector<std::string>& args, RunCallbacks callbacks, RunOptions options) {
    if (args.empty() || args.front().empty() || !hasAnyCallback(callbacks)) {
      return false;
    }

    try {
      std::thread([args, callbacks = std::move(callbacks), options]() mutable {
        if (!callbacks.onExit) {
          options.maxOutputBytes = 0;
        }
        RunResult result = runSyncProcess(args, options, &callbacks);
        if (callbacks.onExit) {
          try {
            callbacks.onExit(std::move(result));
          } catch (...) {
          }
        }
      }).detach();
    } catch (...) {
      return false;
    }

    return true;
  }

  bool runAsync(std::initializer_list<const char*> args) {
    const auto command = makeCommand(args);
    return command.has_value() && runAsync(*command);
  }

  bool runAsync(std::initializer_list<const char*> args, RunCallbacks callbacks, RunOptions options) {
    const auto command = makeCommand(args);
    return command.has_value() && runAsync(*command, std::move(callbacks), options);
  }

  bool runAsync(const std::string& command) {
    if (command.empty()) {
      return false;
    }
    return runAsync(std::vector<std::string>{"/bin/sh", "-lc", command});
  }

  bool runAsync(const std::string& command, RunCallbacks callbacks, RunOptions options) {
    if (command.empty()) {
      return false;
    }
    return runAsync(std::vector<std::string>{"/bin/sh", "-lc", command}, std::move(callbacks), options);
  }

  std::optional<int> launchDetachedTracked(const std::vector<std::string>& args) {
    if (args.empty() || args.front().empty()) {
      return std::nullopt;
    }
    pid_t reported = -1;
    if (!doubleForkExecDetached(args, &reported, {})) {
      return std::nullopt;
    }
    return static_cast<int>(reported);
  }

  std::optional<int> launchDetachedTracked(std::initializer_list<const char*> args) {
    const auto command = makeCommand(args);
    if (!command.has_value()) {
      return std::nullopt;
    }
    return launchDetachedTracked(*command);
  }

  void terminateTracked(int pid) {
    if (pid <= 0) {
      return;
    }
    const auto p = static_cast<pid_t>(pid);
    ::kill(p, SIGTERM);
    int status = 0;
    if (::waitpid(p, &status, WNOHANG) != p) {
      ::kill(p, SIGKILL);
      ::waitpid(p, &status, 0);
    }
  }

  RunResult runSync(const std::vector<std::string>& args) { return runSyncProcess(args, {}); }

  RunResult runSync(const std::vector<std::string>& args, RunOptions options) { return runSyncProcess(args, options); }

  RunResult runSync(std::initializer_list<const char*> args) {
    const auto command = makeCommand(args);
    return command.has_value() ? runSync(*command) : RunResult{-1, {}, {}};
  }

  RunResult runSyncWithTimeout(const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
    RunOptions options;
    options.timeout = timeout;
    return runSyncProcess(args, options);
  }

  RunResult runSyncWithTimeout(std::initializer_list<const char*> args, std::chrono::milliseconds timeout) {
    const auto command = makeCommand(args);
    return command.has_value() ? runSyncWithTimeout(*command, timeout) : RunResult{-1, {}, {}};
  }

  RunResult runSyncWithTimeoutAndOutputLimit(
      const std::vector<std::string>& args, std::chrono::milliseconds timeout, std::size_t maxOutputBytes
  ) {
    RunOptions options;
    options.timeout = timeout;
    options.maxOutputBytes = maxOutputBytes;
    return runSyncProcess(args, options);
  }

  bool commandLineMatchesAll(const std::vector<std::string>& needles) {
    if (needles.empty()) {
      return false;
    }
    if (std::ranges::any_of(needles, [](const auto& needle) { return needle.empty(); })) {
      return false;
    }

    const auto& commandLines = cachedProcessCommandLines();
    return std::ranges::any_of(commandLines, [&needles](const auto& commandLine) {
      return std::ranges::all_of(needles, [&commandLine](const auto& needle) {
        return commandLine.find(needle) != std::string::npos;
      });
    });
  }

  bool desktopPortalAvailable() {
    const bool portal = cachedProcessMatchesAny({"xdg-desktop-portal "});
    if (!portal) {
      return false;
    }
    return cachedProcessMatchesAny(
        {"xdg-desktop-portal-wlr ", "xdg-desktop-portal-hyprland ", "xdg-desktop-portal-gnome ",
         "xdg-desktop-portal-kde "}
    );
  }

  bool flatpakAppInstalled(std::string_view appId) {
    if (!isSafeFlatpakAppId(appId)) {
      return false;
    }

    std::vector<std::filesystem::path> roots;
    appendFlatpakDataRoots(roots);

    std::error_code ec;
    const std::filesystem::path app(appId);
    for (const auto& root : roots) {
      if (std::filesystem::exists(root / "flatpak/app" / app, ec) && !ec) {
        return true;
      }
      ec.clear();
    }
    return false;
  }

  RunResult runSync(const std::string& command) {
    if (command.empty())
      return {-1, {}, {}};
    return runSync(std::vector<std::string>{"/bin/sh", "-lc", command});
  }

  bool launchFirstAvailable(std::initializer_list<std::initializer_list<const char*>> commandVariants) {
    for (const auto& variant : commandVariants) {
      const auto command = makeCommand(variant);
      if (!command.has_value() || command->empty()) {
        continue;
      }
      if (!commandExists(command->front().c_str())) {
        continue;
      }
      if (runAsync(*command)) {
        return true;
      }
    }
    return false;
  }

  bool cgroupIndicatesSystemdUserManager(std::string_view cgroupFileContents, unsigned int uid) {
    // Matches both the cgroup v2 "0::<path>" line and legacy v1 "<id>:<controller>:<path>" lines.
    return cgroupFileContents.contains(std::format("/user@{}.service/", uid));
  }

  bool runningUnderSystemdUserManager() {
#ifdef __linux__
    // A process cannot migrate between the login session scope and the user manager, so probe once.
    static const bool managed = [] {
      std::ifstream file("/proc/self/cgroup");
      if (!file) {
        return false;
      }
      const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      return cgroupIndicatesSystemdUserManager(contents, ::getuid());
    }();
    return managed;
#else
    return false;
#endif
  }

  bool runAsyncAsSystemdService(
      const std::vector<std::string>& args, const std::string& appName, const std::string& activationToken,
      const std::string& workingDir
  ) {
#ifdef __linux__
    if (args.empty() || args.front().empty()) {
      return false;
    }
    if (runningUnderSystemdUserManager()) {
      return startSystemdService(args, activationToken, workingDir, appName);
    }
#endif
    return runAsync(args, activationToken, workingDir);
  }
} // namespace process
