#pragma once

#include "capture/screencopy_capture.h"

#include <chrono>
#include <functional>
#include <string>

namespace screencopy {

  // How long a blocking capture waits for the compositor before giving up.
  inline constexpr std::chrono::milliseconds kBlockingCaptureTimeout{2000};

  enum class WaitOutcome { Completed, TimedOut, Error };

  // The wait side of a blocking capture, kept free of Wayland types so the
  // deadline arithmetic is testable without a compositor.
  //
  // `waitAndDispatch` must block for AT MOST the timeout it is handed — the
  // deadline can only be honoured if the wait itself is bounded — and return
  // >0 when it dispatched events, 0 when the wait expired (or was interrupted)
  // with no progress, <0 on a connection error.
  struct EventWaitOps {
    std::function<int(std::chrono::milliseconds)> waitAndDispatch;
    std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };
  };

  [[nodiscard]] WaitOutcome waitForCapture(
      const EventWaitOps& ops, const std::function<bool()>& done, std::chrono::steady_clock::time_point deadline
  );

  // The capture side, likewise free of Wayland types.
  struct BlockingCaptureOps {
    std::function<void(ScreencopyCapture::CompletionCallback)> start;
    std::function<bool()> busy;
    std::function<void()> cancel;
  };

  // Starts a capture and pumps `wait` until it settles or `timeout` elapses.
  // On timeout or dispatch error the in-flight capture is cancelled and the
  // completion callback is abandoned: a late completion writes nothing.
  [[nodiscard]] bool runBlockingCapture(
      const BlockingCaptureOps& capture, const EventWaitOps& wait, ScreencopyImage& out, std::string& error,
      std::chrono::milliseconds timeout = kBlockingCaptureTimeout
  );

} // namespace screencopy
