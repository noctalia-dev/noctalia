#include "core/process.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "process_test: %s\n", message);
    }
    return condition;
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
      std::lock_guard lock(mutex);
      stdOut.append(chunk);
    };
    callbacks.stdErr = [&](std::string_view chunk) {
      std::lock_guard lock(mutex);
      stdErr.append(chunk);
    };
    callbacks.onExit = [&](process::RunResult value) {
      std::lock_guard lock(mutex);
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
      std::lock_guard lock(mutex);
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
        {"/bin/sh", "-lc", "printf '%s/%s' \"$NOCTALIA_PROCESS_SET_TEST\" \"${NOCTALIA_PROCESS_UNSET_TEST-unset}\""},
        options
    );
    ::unsetenv("NOCTALIA_PROCESS_UNSET_TEST");

    bool ok = expect(result.exitCode == 0, "sync env override command failed");
    ok = expect(result.out == "child/unset", "sync env overrides were not visible in child") && ok;
    return ok;
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

} // namespace

int main() {
  bool ok = true;
  ok = expect(!process::runAsync("true", process::RunCallbacks{}), "empty callback set should not launch") && ok;
  ok = capturedAsyncDeliversCallbacksAndResult() && ok;
  ok = capturedAsyncDeliversCompletionOnly() && ok;
  ok = syncAppliesEnvOverrides() && ok;
  ok = commandExistsRejectsDirectories() && ok;
  return ok ? 0 : 1;
}
