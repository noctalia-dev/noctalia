#pragma once

#include <string>

// Process-lifetime single-instance guard backed by flock(2).
//
// Acquiring the lock is atomic: a second instance racing startup gets a clean
// "already held" answer with no time-of-check/time-of-use window. The kernel
// releases the lock automatically when the holding process dies, so a crashed
// instance never leaves a stale lock behind — no manual cleanup, no PID probing.
//
// The lock is scoped per Wayland display (matching the IPC socket naming) and is
// independent of IPC: it exists purely to answer "am I the only noctalia on this
// display?" and must be claimed before any shell/Wayland init so the answer is
// settled before bars or surfaces are created.
class SingleInstanceLock {
public:
  SingleInstanceLock() = default;
  ~SingleInstanceLock();

  SingleInstanceLock(const SingleInstanceLock&) = delete;
  SingleInstanceLock& operator=(const SingleInstanceLock&) = delete;

  // Attempts to acquire the lock. Returns true if we now hold it (we are the only
  // instance) and false if another live instance already holds it. If the lock
  // file cannot be opened at all (e.g. an unwritable runtime dir), startup is
  // allowed to proceed unguarded rather than bricking the shell — this is logged.
  bool tryAcquire();

  bool held() const noexcept { return m_fd >= 0; }
  const std::string& path() const noexcept { return m_path; }

private:
  void release() noexcept;
  static std::string resolveLockPath();

  int m_fd = -1;
  std::string m_path;
};
