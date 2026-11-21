#pragma once

#include "wayland/wayland_seat.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

class WaylandConnection;

class LockKeysService {
public:
  using LockKeysState = WaylandSeat::LockKeysState;
  using ChangeCallback = std::function<void(const LockKeysState& previous, const LockKeysState& current)>;

  explicit LockKeysService(WaylandConnection& wayland);
  ~LockKeysService();
  LockKeysService(const LockKeysService&) = delete;
  LockKeysService& operator=(const LockKeysService&) = delete;

  LockKeysState state() const noexcept;
  void setChangeCallback(ChangeCallback callback);
  int pollTimeoutMs() const;
  void dispatchPoll();
  void refreshNow();

private:
  LockKeysState readCurrentState();
  std::optional<LockKeysState> readCachedSysfsState();
  void discoverSysfsLeds();
  bool hasCachedSysfsLeds() const noexcept;
  void closeSysfsLeds() noexcept;

  WaylandConnection& m_wayland;
  LockKeysState m_state;
  bool m_hasState = false;
  bool m_sysfsDiscovered = false;
  int m_sysfsReadFailures = 0;
  std::chrono::steady_clock::time_point m_nextRefreshAt;
  ChangeCallback m_changeCallback;
  // Open LED brightness files, re-read on every poll.
  std::vector<int> m_capsLockFds;
  std::vector<int> m_numLockFds;
  std::vector<int> m_scrollLockFds;
};
