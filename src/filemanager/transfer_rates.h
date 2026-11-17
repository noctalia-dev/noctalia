#pragma once

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cstdint>
#include <deque>
#include <limits>

namespace filemanager {
// Cumulative byte counters, never averages of already calculated rates/ETAs.
class TransferRates {
public:
  using Clock = std::chrono::steady_clock;
  void observe(Clock::time_point now, std::uint64_t bytes, bool paused) {
    if (!m_samples.empty() && now < m_samples.back().time) return;
    if (m_samples.empty() || paused != m_paused || bytes < m_samples.back().bytes) {
      m_samples.clear();
      m_paused = paused;
    }
    if (paused) { m_samples = {{now, bytes}}; return; }
    if (!m_samples.empty() && now == m_samples.back().time) m_samples.back().bytes = bytes;
    else m_samples.push_back({now, bytes});
    const auto cutoff = now - std::chrono::seconds(5);
    // Retain one sample on/before the boundary for interpolation.
    while (m_samples.size() > 2 && m_samples[1].time <= cutoff) m_samples.pop_front();
  }
  [[nodiscard]] std::uint64_t displaySpeed() const { return rate(std::chrono::seconds(1)); }
  [[nodiscard]] std::uint64_t etaSpeed() const { return rate(std::chrono::seconds(5)); }
private:
  struct Sample { Clock::time_point time; std::uint64_t bytes; };
  [[nodiscard]] std::uint64_t rate(Clock::duration window) const {
    if (m_paused || m_samples.size() < 2) return 0;
    const auto& last = m_samples.back();
    const auto start = std::max(m_samples.front().time, last.time - window);
    const auto seconds = std::chrono::duration<long double>(last.time - start).count();
    if (seconds <= 0) return 0;
    auto right = std::upper_bound(m_samples.begin(), m_samples.end(), start,
        [](const auto& time, const Sample& sample) { return time < sample.time; });
    const auto& left = right == m_samples.begin() ? *right : *std::prev(right);
    long double startBytes = static_cast<long double>(left.bytes);
    if (right != m_samples.end() && right->time > left.time) {
      const auto fraction = std::chrono::duration<long double>(start - left.time).count()
          / std::chrono::duration<long double>(right->time - left.time).count();
      startBytes += fraction * static_cast<long double>(right->bytes - left.bytes);
    }
    const auto speed = std::max(0.0L, (static_cast<long double>(last.bytes) - startBytes) / seconds);
    return speed >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(speed);
  }
  std::deque<Sample> m_samples;
  bool m_paused = false;
};
} // namespace filemanager
