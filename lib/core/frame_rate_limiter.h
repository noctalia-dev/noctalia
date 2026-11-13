#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <functional>

// Caps animation-driven repaints to a fixed rate regardless of the compositor's
// frame-callback rate. Animated widgets (e.g. the scrolling system graphs)
// repaint from the per-frame tick, which fires at the display's refresh rate; on
// high-refresh, high-resolution displays that repaints a large blurred surface
// far more often than the motion needs, burning GPU/CPU for no visible gain.
//
// Gate the per-frame work through shouldStep(): it returns true at most once per
// interval. When it returns false it arms a one-shot timer that re-requests a
// redraw near the interval edge, so the animation still advances at the capped
// rate even after the frame loop would otherwise go idle (the loop is sustained
// by requestRedraw(), so a skipped frame that issues no redraw stops it).
class FrameRateLimiter {
public:
  explicit FrameRateLimiter(std::chrono::milliseconds interval = std::chrono::milliseconds{33})
      : m_interval(interval) {}

  // Call from a per-frame tick. Returns true when enough time has elapsed to run
  // another step; otherwise arms reviveCb to fire near the interval edge and
  // returns false so the caller skips this frame's update + redraw.
  bool shouldStep(std::function<void()> reviveCb) {
    const auto now = std::chrono::steady_clock::now();
    const auto since = now - m_lastStepAt;
    if (since < m_interval) {
      if (!m_revive.active()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_interval - since);
        m_revive.start(
            remaining < std::chrono::milliseconds{1} ? std::chrono::milliseconds{1} : remaining, std::move(reviveCb)
        );
      }
      return false;
    }
    m_lastStepAt = now;
    return true;
  }

  // Cancels any pending revive and lets the next shouldStep() run immediately.
  void reset() {
    m_lastStepAt = {};
    m_revive.stop();
  }

private:
  std::chrono::milliseconds m_interval;
  std::chrono::steady_clock::time_point m_lastStepAt;
  Timer m_revive;
};
