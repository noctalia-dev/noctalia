#pragma once

#include <functional>
#include <unordered_map>

// Reports once, the first time every live member has shown a frame. Members destroyed before that no longer count.
// The tracker must outlive its members.
class FirstFrameTracker {
public:
  class Member {
  public:
    explicit Member(FirstFrameTracker& tracker) : m_tracker(tracker) { m_tracker.add(this); }
    ~Member() { m_tracker.remove(this); }

    Member(const Member&) = delete;
    Member& operator=(const Member&) = delete;

    void shown() { m_tracker.shown(this); }

  private:
    FirstFrameTracker& m_tracker;
  };

  void setCallback(std::function<void()> callback) { m_callback = std::move(callback); }

  [[nodiscard]] bool done() const noexcept { return m_done || m_shown.empty(); }

private:
  void add(const Member* member) {
    if (!m_done) {
      m_shown.try_emplace(member, false);
    }
  }

  void remove(const Member* member) {
    if (!m_done && m_shown.erase(member) > 0) {
      completeIfAllShown();
    }
  }

  void shown(const Member* member) {
    if (m_done) {
      return;
    }
    auto it = m_shown.find(member);
    if (it == m_shown.end()) {
      return;
    }
    it->second = true;
    completeIfAllShown();
  }

  void completeIfAllShown() {
    for (const auto& [member, shown] : m_shown) {
      if (!shown) {
        return;
      }
    }
    m_done = true;
    m_shown.clear();
    if (m_callback) {
      m_callback();
    }
  }

  std::unordered_map<const Member*, bool> m_shown;
  std::function<void()> m_callback;
  bool m_done = false;
};
