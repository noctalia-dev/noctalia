// Blocking screencopy: the wait is bounded by a deadline the compositor cannot
// outrun, the pending frame is cancelled when we give up, and a completion
// that arrives late has nothing of the caller's left to write to.

#include "capture/screencopy_blocking.h"

#include <chrono>
#include <functional>
#include <optional>
#include <print>
#include <string>
#include <vector>

namespace {

  using namespace std::chrono_literals;

  int g_failures = 0;

  void expectTrue(const char* what, bool cond) {
    if (!cond) {
      std::println(stderr, "screencopy_blocking_test: FAIL: {}", what);
      ++g_failures;
    }
  }

  void expectEqual(const char* what, const std::string& actual, const std::string& expected) {
    if (actual != expected) {
      std::println(stderr, "screencopy_blocking_test: FAIL: {} = \"{}\", expected \"{}\"", what, actual, expected);
      ++g_failures;
    }
  }

  void expectEqual(const char* what, std::size_t actual, std::size_t expected) {
    if (actual != expected) {
      std::println(stderr, "screencopy_blocking_test: FAIL: {} = {}, expected {}", what, actual, expected);
      ++g_failures;
    }
  }

  [[nodiscard]] ScreencopyImage sampleImage() {
    ScreencopyImage image;
    image.width = 2;
    image.height = 1;
    image.rgba.assign(2U * 1U * 4U, 0xFFU);
    return image;
  }

  // A compositor stand-in driven by a virtual clock: every wait consumes
  // exactly the timeout it was handed, so "the deadline is respected" is a
  // deterministic assertion rather than a race with wall time.
  struct FakeLoop {
    std::chrono::steady_clock::time_point clock{};
    std::vector<std::chrono::milliseconds> waits;
    int pumpsUntilCompletion = -1; // <0: never completes
    int failAfterPumps = -1;       // >=0: waitAndDispatch reports a connection error
    std::function<void()> onProgress;

    [[nodiscard]] screencopy::EventWaitOps ops() {
      return screencopy::EventWaitOps{
          .waitAndDispatch =
              [this](std::chrono::milliseconds timeout) {
                waits.push_back(timeout);
                clock += timeout;
                if (failAfterPumps >= 0 && static_cast<int>(waits.size()) > failAfterPumps) {
                  return -1;
                }
                if (pumpsUntilCompletion >= 0 && static_cast<int>(waits.size()) >= pumpsUntilCompletion) {
                  if (onProgress) {
                    onProgress();
                  }
                  return 1;
                }
                return 0;
              },
          .now = [this] { return clock; },
      };
    }
  };

  struct FakeCapture {
    ScreencopyCapture::CompletionCallback callback;
    bool busy = false;
    int cancels = 0;
    bool completeOnStart = false;
    std::string startError;

    [[nodiscard]] screencopy::BlockingCaptureOps ops() {
      return screencopy::BlockingCaptureOps{
          .start =
              [this](ScreencopyCapture::CompletionCallback onComplete) {
                callback = std::move(onComplete);
                busy = true;
                if (!startError.empty()) {
                  busy = false;
                  callback(std::nullopt, startError);
                  return;
                }
                if (completeOnStart) {
                  busy = false;
                  callback(sampleImage(), {});
                }
              },
          .busy = [this] { return busy; },
          .cancel =
              [this] {
                ++cancels;
                busy = false;
              },
      };
    }

    void complete() {
      busy = false;
      callback(sampleImage(), {});
    }
  };

} // namespace

int main() {
  // A capture that completes on the first pump succeeds and yields its frame.
  {
    FakeCapture capture;
    FakeLoop loop;
    loop.pumpsUntilCompletion = 1;
    loop.onProgress = [&capture] { capture.complete(); };

    ScreencopyImage out;
    std::string error;
    const bool ok = screencopy::runBlockingCapture(capture.ops(), loop.ops(), out, error, 2000ms);

    expectTrue("completed capture returns true", ok);
    expectEqual("completed capture leaves no error", error, "");
    expectTrue("completed capture fills the frame", out.width == 2 && out.height == 1 && !out.rgba.empty());
    expectEqual("completed capture does not cancel", static_cast<std::size_t>(capture.cancels), 0U);
  }

  // A completion that fires synchronously from start() needs no pumping.
  {
    FakeCapture capture;
    capture.completeOnStart = true;
    FakeLoop loop;

    ScreencopyImage out;
    std::string error;
    const bool ok = screencopy::runBlockingCapture(capture.ops(), loop.ops(), out, error, 2000ms);

    expectTrue("synchronous completion returns true", ok);
    expectEqual("synchronous completion never waits", loop.waits.size(), 0U);
  }

  // The reported bug: the compositor never answers. The wait must end at the
  // deadline, cancel the frame, and never ask to wait past the budget.
  {
    FakeCapture capture;
    FakeLoop loop; // never completes

    ScreencopyImage out;
    std::string error;
    const bool ok = screencopy::runBlockingCapture(capture.ops(), loop.ops(), out, error, 2000ms);

    expectTrue("silent compositor returns false", !ok);
    expectEqual("silent compositor reports a timeout", error, "screencopy capture timed out");
    expectEqual("silent compositor cancels the frame", static_cast<std::size_t>(capture.cancels), 1U);
    expectTrue("the wait ends at the deadline", loop.clock <= std::chrono::steady_clock::time_point{} + 2000ms);

    std::chrono::milliseconds total{0};
    for (const auto wait : loop.waits) {
      total += wait;
    }
    expectTrue("no wait exceeds the remaining budget", total <= 2000ms);
    expectTrue("the deadline is reached, not merely approached", total == 2000ms);
  }

  // A wait that blocks for the whole budget in one go still terminates: the
  // deadline is re-checked after the wait returns, not only between pumps.
  {
    FakeCapture capture;
    FakeLoop loop;

    ScreencopyImage out;
    std::string error;
    const bool ok = screencopy::runBlockingCapture(capture.ops(), loop.ops(), out, error, 50ms);

    expectTrue("single long wait returns false", !ok);
    expectEqual("single long wait times out", error, "screencopy capture timed out");
    expectEqual("single long wait pumps exactly once", loop.waits.size(), 1U);
    expectTrue("single long wait is bounded by the timeout", loop.waits.front() == 50ms);
  }

  // A dead Wayland connection is an error, not a hang.
  {
    FakeCapture capture;
    FakeLoop loop;
    loop.failAfterPumps = 0;

    ScreencopyImage out;
    std::string error;
    const bool ok = screencopy::runBlockingCapture(capture.ops(), loop.ops(), out, error, 2000ms);

    expectTrue("dispatch failure returns false", !ok);
    expectEqual("dispatch failure is reported", error, "Wayland event dispatch failed");
    expectEqual("dispatch failure cancels the frame", static_cast<std::size_t>(capture.cancels), 1U);
  }

  // The capture failing outright surfaces the compositor's message.
  {
    FakeCapture capture;
    capture.startError = "screencopy unavailable";
    FakeLoop loop;

    ScreencopyImage out;
    std::string error;
    const bool ok = screencopy::runBlockingCapture(capture.ops(), loop.ops(), out, error, 2000ms);

    expectTrue("failed capture returns false", !ok);
    expectEqual("failed capture keeps the reason", error, "screencopy unavailable");
  }

  // What the timeout is really guarding: a completion that arrives after we
  // gave up. The callback must touch nothing the caller owns — before the fix
  // it held references to `out` and `error` and wrote through them.
  {
    FakeCapture capture;
    FakeLoop loop;

    ScreencopyImage out;
    std::string error;
    const bool ok = screencopy::runBlockingCapture(capture.ops(), loop.ops(), out, error, 2000ms);
    expectTrue("abandoned capture times out", !ok && error == "screencopy capture timed out");

    // The compositor finally answers, long after runBlockingCapture returned.
    capture.callback(sampleImage(), {});

    expectEqual("late completion does not overwrite the caller's error", error, "screencopy capture timed out");
    expectTrue("late completion does not fill the caller's frame", out.width == 0 && out.rgba.empty());
  }

  if (g_failures > 0) {
    std::println(stderr, "screencopy_blocking_test: {} failure(s)", g_failures);
    return 1;
  }
  return 0;
}
