#include "capture/screencopy_blocking.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace screencopy {

  WaitOutcome waitForCapture(
      const EventWaitOps& ops, const std::function<bool()>& done, std::chrono::steady_clock::time_point deadline
  ) {
    while (!done()) {
      const auto now = ops.now();
      if (now >= deadline) {
        return WaitOutcome::TimedOut;
      }

      // Hand the wait what is left of the budget, never more: this is the only
      // thing standing between a silent compositor and a frozen main loop.
      const auto remaining =
          std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), std::chrono::milliseconds{0});
      if (ops.waitAndDispatch(remaining) < 0) {
        // The connection may still have handed us the completion on its way
        // out — take it rather than reporting a failure we already recovered.
        return done() ? WaitOutcome::Completed : WaitOutcome::Error;
      }
    }
    return WaitOutcome::Completed;
  }

  bool runBlockingCapture(
      const BlockingCaptureOps& capture, const EventWaitOps& wait, ScreencopyImage& out, std::string& error,
      std::chrono::milliseconds timeout
  ) {
    error.clear();

    // Heap state, not the caller's stack: once we give up (timeout, dispatch
    // error) a completion that still arrives must have nothing of ours left to
    // write to. `abandoned` turns that late fire into a no-op instead of a
    // use-after-return on `out` and `error`.
    struct State {
      bool finished = false;
      bool abandoned = false;
      std::optional<ScreencopyImage> image;
      std::string error;
    };
    const auto state = std::make_shared<State>();

    capture.start([state](std::optional<ScreencopyImage> image, std::string err) {
      if (state->abandoned) {
        return;
      }
      state->finished = true;
      if (!err.empty() || !image.has_value()) {
        state->error = err.empty() ? "screencopy capture failed" : std::move(err);
        return;
      }
      state->image = std::move(image);
    });

    const auto done = [&state, &capture] { return state->finished || !capture.busy(); };
    const auto outcome = waitForCapture(wait, done, wait.now() + timeout);

    switch (outcome) {
    case WaitOutcome::Completed:
      break;
    case WaitOutcome::TimedOut:
      state->abandoned = true;
      capture.cancel();
      error = "screencopy capture timed out";
      return false;
    case WaitOutcome::Error:
      state->abandoned = true;
      capture.cancel();
      error = "Wayland event dispatch failed";
      return false;
    }

    if (!state->error.empty()) {
      error = state->error;
      return false;
    }
    if (!state->finished || !state->image.has_value()) {
      error = "screencopy capture failed";
      return false;
    }

    out = std::move(*state->image);
    if (out.width <= 0 || out.height <= 0 || out.rgba.empty()) {
      error = "screencopy capture returned an empty frame";
      return false;
    }
    return true;
  }

} // namespace screencopy
