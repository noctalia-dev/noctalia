#include "core/process/process.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "process_test: {}", message);
    }
    return condition;
  }

  std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (const char ch : value) {
      if (ch == '\'') {
        quoted += "'\\''";
      } else {
        quoted += ch;
      }
    }
    quoted += "'";
    return quoted;
  }

  bool capturedAsyncDeliversCallbacksAndResult() {
    std::mutex mutex;
    std::condition_variable cv;
    std::string stdOut;
    std::string stdErr;
    std::optional<process::RunResult> result;
    bool completed = false;

    process::RunCallbacks callbacks;
    callbacks.stdOut = [&](std::string_view chunk) {
      std::scoped_lock lock(mutex);
      stdOut.append(chunk);
    };
    callbacks.stdErr = [&](std::string_view chunk) {
      std::scoped_lock lock(mutex);
      stdErr.append(chunk);
    };
    callbacks.onExit = [&](process::RunResult value) {
      std::scoped_lock lock(mutex);
      result = std::move(value);
      completed = true;
      cv.notify_one();
    };

    process::RunOptions options;
    options.timeout = std::chrono::seconds(2);
    options.maxOutputBytes = 3;

    const bool launched =
        process::runAsync({"/bin/sh", "-lc", "printf abcdef; printf XYZ >&2"}, std::move(callbacks), options);
    if (!expect(launched, "captured async command did not launch")) {
      return false;
    }

    std::unique_lock lock(mutex);
    bool ok = expect(
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }), "captured async command did not complete"
    );
    ok = expect(result.has_value(), "captured async command did not provide a result") && ok;
    ok = expect(stdOut == "abcdef", "stdout callback did not receive full output") && ok;
    ok = expect(stdErr == "XYZ", "stderr callback did not receive full output") && ok;
    if (result.has_value()) {
      ok = expect(result->exitCode == 0, "captured async exit code was not zero") && ok;
      ok = expect(result->out == "abc", "captured async stdout result did not respect output limit") && ok;
      ok = expect(result->err == "XYZ", "captured async stderr result was wrong") && ok;
      ok = expect(result->outTruncated, "captured async stdout result was not marked truncated") && ok;
      ok = expect(!result->errTruncated, "captured async stderr result was incorrectly marked truncated") && ok;
      ok = expect(!result->timedOut, "captured async command timed out unexpectedly") && ok;
    }
    return ok;
  }

  bool capturedAsyncDeliversCompletionOnly() {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<process::RunResult> result;
    bool completed = false;

    process::RunCallbacks callbacks;
    callbacks.onExit = [&](process::RunResult value) {
      std::scoped_lock lock(mutex);
      result = std::move(value);
      completed = true;
      cv.notify_one();
    };

    const bool launched = process::runAsync("printf ok; exit 7", std::move(callbacks));
    if (!expect(launched, "completion-only async command did not launch")) {
      return false;
    }

    std::unique_lock lock(mutex);
    bool ok = expect(
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }),
        "completion-only async command did not complete"
    );
    ok = expect(result.has_value(), "completion-only async command did not provide a result") && ok;
    if (result.has_value()) {
      ok = expect(result->exitCode == 7, "completion-only async command exit code was wrong") && ok;
      ok = expect(result->out == "ok", "completion-only async command stdout was wrong") && ok;
      ok = expect(result->err.empty(), "completion-only async command stderr was not empty") && ok;
    }
    return ok;
  }

  bool syncAppliesEnvOverrides() {
    ::setenv("NOCTALIA_PROCESS_UNSET_TEST", "parent", 1);

    process::RunOptions options;
    options.env.push_back({"NOCTALIA_PROCESS_SET_TEST", "child"});
    options.env.push_back({"NOCTALIA_PROCESS_UNSET_TEST", std::nullopt});

    const auto result = process::runSync(
        {"/bin/sh", "-lc", R"(printf '%s/%s' "$NOCTALIA_PROCESS_SET_TEST" "${NOCTALIA_PROCESS_UNSET_TEST-unset}")"},
        options
    );
    ::unsetenv("NOCTALIA_PROCESS_UNSET_TEST");

    bool ok = expect(result.exitCode == 0, "sync env override command failed");
    ok = expect(result.out == "child/unset", "sync env overrides were not visible in child") && ok;
    return ok;
  }

  bool stringCommandsSupportShellComposition() {
    const auto result = process::runSync("printf first && printf second");
    bool ok = expect(result.exitCode == 0, "composed shell command failed");
    ok = expect(result.out == "firstsecond", "composed shell command did not execute both commands") && ok;
    return ok;
  }

  bool detachedAsyncInheritsLaunchEnvironment() {
    const std::filesystem::path outPath =
        std::filesystem::temp_directory_path() / ("noctalia_process_env_test_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    ::setenv("NOCTALIA_WALLPAPER_PATH", "/tmp/noctalia test/wallpaper.png", 1);
    ::setenv("NOCTALIA_WALLPAPER_CONNECTOR", "DP-1", 1);

    const std::string command = R"(printf '%s\n%s' "$NOCTALIA_WALLPAPER_PATH" "$NOCTALIA_WALLPAPER_CONNECTOR" > )"
        + shellQuote(outPath.string());
    const bool launched = process::runAsync(command);
    ::unsetenv("NOCTALIA_WALLPAPER_PATH");
    ::unsetenv("NOCTALIA_WALLPAPER_CONNECTOR");

    if (!expect(launched, "detached async env command did not launch")) {
      return false;
    }

    std::string contents;
    for (int i = 0; i < 50; ++i) {
      std::ifstream in(outPath);
      contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (contents == "/tmp/noctalia test/wallpaper.png\nDP-1") {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::filesystem::remove(outPath, ec);
    return expect(contents == "/tmp/noctalia test/wallpaper.png\nDP-1", "detached async env was not visible in child");
  }

  bool commandExistsRejectsDirectories() {
    bool ok = true;
    ok =
        expect(!process::commandExists("/usr/bin"), "commandExists should return false for /usr/bin (directory)") && ok;
    ok = expect(!process::commandExists("/"), "commandExists should return false for / (directory)") && ok;
    ok = expect(process::commandExists("true"), "commandExists should return true for 'true' on PATH") && ok;
    ok = expect(!process::commandExists(""), "commandExists should return false for empty string") && ok;
    ok =
        expect(!process::commandExists("/nonexistent"), "commandExists should return false for nonexistent path") && ok;
    return ok;
  }

  bool cgroupDetectsSystemdUserManager() {
    bool ok = true;
    ok = expect(
             process::cgroupIndicatesSystemdUserManager(
                 "0::/user.slice/user-1000.slice/user@1000.service/session.slice/wayland-wm@niri.service\n", 1000
             ),
             "uwsm compositor unit should be detected as user-manager managed"
         )
        && ok;
    ok = expect(
             process::cgroupIndicatesSystemdUserManager(
                 "0::/user.slice/user-1000.slice/user@1000.service/app.slice/noctalia.service\n", 1000
             ),
             "noctalia user service should be detected as user-manager managed"
         )
        && ok;
    ok = expect(
             !process::cgroupIndicatesSystemdUserManager(
                 "0::/user.slice/user-1000.slice/session-2.scope/noctalia\n", 1000
             ),
             "login session scope should not be detected as user-manager managed"
         )
        && ok;
    ok = expect(
             !process::cgroupIndicatesSystemdUserManager(
                 "0::/user.slice/user-1001.slice/user@1001.service/app.slice/noctalia.service\n", 1000
             ),
             "another user's manager should not be detected as ours"
         )
        && ok;
    ok = expect(
             process::cgroupIndicatesSystemdUserManager(
                 "1:name=systemd:/user.slice/user-1000.slice/user@1000.service/app.slice/noctalia.service\n", 1000
             ),
             "legacy cgroup v1 dump should be detected as user-manager managed"
         )
        && ok;
    ok = expect(!process::cgroupIndicatesSystemdUserManager("", 1000), "empty cgroup dump should not be managed") && ok;
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = expect(!process::runAsync("true", process::RunCallbacks{}), "empty callback set should not launch") && ok;
  ok = capturedAsyncDeliversCallbacksAndResult() && ok;
  ok = capturedAsyncDeliversCompletionOnly() && ok;
  ok = syncAppliesEnvOverrides() && ok;
  ok = stringCommandsSupportShellComposition() && ok;
  ok = detachedAsyncInheritsLaunchEnvironment() && ok;
  ok = commandExistsRejectsDirectories() && ok;
  ok = cgroupDetectsSystemdUserManager() && ok;
  return ok ? 0 : 1;
}
