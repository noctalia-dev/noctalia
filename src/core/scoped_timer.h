#pragma once

#include "core/log.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

// Lightweight, opt-in wall-clock profiling. Output is gated behind the
// NOCTALIA_PROFILE env var so normal runs stay silent; set it to any non-empty
// value to surface timing lines (e.g. `NOCTALIA_PROFILE=1 noctalia`).
namespace noctalia::profiling {

  // Evaluated once on first use.
  inline bool enabled() {
    static const bool value = [] {
      const char* v = std::getenv("NOCTALIA_PROFILE");
      return v != nullptr && v[0] != '\0';
    }();
    return value;
  }

  // Elapsed milliseconds since construction (or last reset).
  class StopWatch {
  public:
    StopWatch() : m_start(std::chrono::steady_clock::now()) {}
    double elapsedMs() const {
      const auto now = std::chrono::steady_clock::now();
      return std::chrono::duration<double, std::milli>(now - m_start).count();
    }
    void reset() { m_start = std::chrono::steady_clock::now(); }

  private:
    std::chrono::steady_clock::time_point m_start;
  };

  // RAII timer: logs "<label>: <ms> ms" at Info on destruction when profiling is
  // enabled, and is a near-free no-op otherwise.
  class ScopedTimer {
  public:
    ScopedTimer(Logger log, std::string label) : m_log(log), m_label(std::move(label)), m_active(enabled()) {}
    ~ScopedTimer() {
      if (m_active) {
        m_log.info("{}: {:.1F} ms", m_label, m_watch.elapsedMs());
      }
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

  private:
    Logger m_log;
    std::string m_label;
    bool m_active;
    StopWatch m_watch;
  };

} // namespace noctalia::profiling
